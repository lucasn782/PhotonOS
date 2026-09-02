# 🌐 PhotonOS — TCP Phase 2A: 3-Way Handshake, State Machine & Connect()

Este documento especifica a arquitetura, implementação e validação experimental da **TCP Phase 2A** no PhotonOS.

---

## 1. Visão Geral e Objetivos

A Fase 2A do subsistema TCP do PhotonOS implementa a capacidade do sistema operacional de iniciar ativamente conexões TCP (*active open*) interoperáveis com endpoints externos (como o host via interface virtual QEMU slirp), em estrita conformidade com a RFC 793.

O fluxo central implementado e comprovado no fio (*wire*) é:

```text
PhotonOS (Client)                       Peer / Host (Server)
   |                                              |
   | [PKT 1] SYN (Seq = ISS, Ack = 0)             |
   |--------------------------------------------->|
   |                                              |
   | [PKT 2] SYN + ACK (Seq = peer_ISS, Ack = ISS+1)|
   |<---------------------------------------------|
   |                                              |
   | [PKT 3] ACK (Seq = ISS+1, Ack = peer_ISS+1)  |
   |--------------------------------------------->|
   v                                              v
ESTABLISHED                                  ESTABLISHED
```

---

## 2. Arquitetura de Componentes

### 2.1. Protocol Control Block (PCB) e Ciclo de Vida
Cada conexão TCP é gerenciada por uma estrutura `struct tcp_pcb` (`include/tcp.h` e `src/kernel/tcp.c`), protegida por locks granulares:
* `tcp_pcbs_lock`: Mutex global que protege a lista encadeada de todos os PCBs registrados no sistema (`tcp_pcbs`).
* `pcb->lock`: Mutex individual por PCB, protegendo campos de estado, números de sequência (`seq_number`, `ack_number`, `iss`, `snd_una`, `snd_nxt`, `rcv_nxt`), janelas de recepção/emissão e temporizadores.

### 2.2. Máquina de Estados Operacional
Nesta fase, as seguintes transições de estado foram formalizadas e implementadas:
* `CLOSED -> SYN_SENT`: Ativado por `sys_connect()`, dispara o segmento inicial `SYN`.
* `SYN_SENT -> ESTABLISHED`: Ativado pelo recebimento de `SYN+ACK` válido no `tcp_input()`, gerando a transmissão imediata do `ACK` final.
* `SYN_SENT -> CLOSED`: Ativado em caso de recebimento de `RST` (porta remota fechada), timeout de conexão ou estouro do limite de retransmissões RTO.
* `ESTABLISHED -> CLOSED`: Ativado pelo encerramento via `tcp_socket_destroy()` / `sys_close()`.

### 2.3. Temporizadores e Retransmissão RTO
* **Temporizador de Conexão (`pcb->timers.timeout`)**: Configurado por `TCP_CONNECT_TIMEOUT_TICKS` (500 ticks, ~500 ms no timer do kernel). Se nenhum `SYN+ACK` for recebido antes de expirar o prazo, a conexão é abortada com transição para `CLOSED` e notificação da tarefa adormecida. O teste de espaço de usuário aguarda até 2000 ms para comprovação da falha.
* **Retransmissão RTO (`pcb->timers.retransmission`)**: Intervalo inicial de `TCP_RTO_TICKS_DEFAULT` (100 ticks, ~100 ms) com recuo exponencial (*exponential backoff* duplicando até o teto de 1000 ticks) e limite de `TCP_MAX_SYN_RETRIES` (3 tentativas de retransmissão).
* **Ausência de Deadlock na Transmissão**: A função `tcp_timer_tick()` utiliza uma fila de transmissão diferida (*deferred queue* de até 4 pacotes). Nenhuma chamada a `net_send_ipv4()` é feita segurando `tcp_pcbs_lock`, prevenindo impasses cíclicos (deadlocks) com a rotina de recepção (`net_poll_packets` -> `tcp_input`).

---

## 3. Integração com a Camada de Sockets e Syscalls

### 3.1. Chamada de Sistema `sys_connect()`
A syscall `sys_connect(int fd, const struct sockaddr *addr, socklen_t addrlen)` em `src/kernel/net.c`:
1. Valida o descritor de arquivo e certifica-se de que se trata de um socket `SOCK_STREAM` (`IPPROTO_TCP`).
2. Associa o endpoint remoto (`remote_ip`, `remote_port`) e atribui uma porta efêmera local no intervalo IANA (`TCP_EPHEMERAL_PORT_FIRST = 49152` a `TCP_EPHEMERAL_PORT_LAST = 65535`).
3. Gera o número de sequência inicial monotônico `ISS` baseado no clock do kernel (`(uint32_t)(kernel_ticks + 1000ULL)`).
4. Inicializa o rastreamento de sequência do PCB:
   * `pcb->iss = iss`
   * `pcb->seq_number = iss`
   * `pcb->snd_una = iss`
   * `pcb->snd_nxt = iss + 1U`
   * `pcb->ack_number = 0`
   * `pcb->window = TCP_DEFAULT_WINDOW (65535)`
5. Transmite o primeiro segmento `SYN` pela rede IPv4/e1000 (`tcp_output(pcb, TCP_FLAG_SYN, 0, 0)`).
6. Coloca a tarefa chamadora em espera cooperativa no escalonador via `scheduler_sleep_current(TASK_WAIT_NETWORK, (uint64_t)sock)` seguido de `scheduler_yield()`. Isso cede o processador para que a thread de rede continue processando pacotes e o escalonador execute outras tarefas, sem espera ocupada ("zero busy-wait").
7. Ao receber o segmento `SYN+ACK`, a rotina de recepção executa `tcp_socket_notify(socket)`, acordando a tarefa. Ao reavaliar o estado do PCB, `sys_connect()` retorna `0` em caso de sucesso (`ESTABLISHED`) ou `-1` em caso de erro (`RST` ou timeout).

---

## 4. Evidência Experimental e Inspeção de Pacotes (PCAP)

A validação foi conduzida via script automatizado `scripts/test_tcp_phase2a.py`, com servidor de eco/escuta no Host Python em `0.0.0.0:8088`, captura de rede completa via `-object filter-dump,file=build/tcp_test.pcap` do QEMU e binário de espaço de usuário `tcptest.elf`.

### 4.1. Resumo dos Casos de Teste

| Test Case | Descrição | Resultado | RTT Médio |
|-----------|-----------|-----------|-----------|
| `HANDSHAKE_ESTABLISHED` | Conexão 10.0.2.15 -> 10.0.2.2:8088 | **PASS** | ~80 ms |
| `CLOSED_PORT_RST` | Conexão contra porta fechada (8099) | **PASS** | Erro imediato via RST |
| `TIMEOUT_HANDLING` | Conexão contra IP inexistente (10.0.2.240) | **PASS** | Abortado por timeout |
| `STRESS_CONSECUTIVE` | 4 conexões consecutivas em sequência | **PASS** | 7 a 82 ms |
| `CONCURRENT_FORK` | 4 conexões simultâneas via `fork()` | **PASS** | 10 a 52 ms |
| `PCAP WIRE INSPECTION` | Verificação dos 3 segmentos no dump | **PASS** | 32 pacotes capturados |

### 4.2. Traço Real dos Três Segmentos (Captura PCAP)
```text
[PKT 1 - SYN]     10.0.2.15:49152 -> 10.0.2.2:8088 | Seq=152862 Ack=0      Flags=0x02 (SYN)
[PKT 2 - SYN+ACK] 10.0.2.2:8088   -> 10.0.2.15:49152 | Seq=64001  Ack=152863 Flags=0x12 (SYN+ACK)
[PKT 3 - ACK]     10.0.2.15:49152 -> 10.0.2.2:8088 | Seq=152863 Ack=64002  Flags=0x10 (ACK)
```
* **Cálculo de Sequência e Acknowledge**: O `ACK` emitido pelo PhotonOS possui `Ack = peer_ISS + 1` (`64002`) e `Seq = ISS + 1` (`152863`), comprovando a conformidade estrita com o protocolo TCP.

---

## 5. Testes de Não-Regressão

A introdução da Fase 2A não causou regressões no ecossistema PhotonOS:
* `test_10_boots.py`: **10/10 PASS** (Inicialização de IDT, PMM, VMM, Heap W^X, FAT16, TCP, SMP e Shell).
* `test_signals_suite.py`: **PASS** (SIGCHLD, SIGPIPE, EOF em Pipes, SIGSTOP/CONT, waitpid(WNOHANG), sigprocmask, SMP stress).
* `test_vfs_qemu.py`: **PASS** (Toda a suite de VFS, montagem dinâmica, dup/fcntl e ping RAW socket ICMP).

---

## 6. Limitações Atuais e Próximos Passos (Fase 2B)

### 6.1. O que NÃO está implementado nesta fase:
* Janela deslizante (*sliding window*) completa e controle de congestionamento (*slow start*, *congestion avoidance*).
* Transferência full-duplex de fluxo contínuo (`sys_send`, `sys_recv`, `read`, `write` sobre sockets conectados).
* Lado passivo do servidor (`listen()`, `accept()`, fila de backlog).
* Encerramento ordenado de quatro vias (`FIN`, `FIN+ACK`, `TIME_WAIT`).

### 6.2. Próxima Etapa Recomendada:
* **TCP Phase 2B**: Implementação das chamadas de fluxo de dados (`send()` e `recv()`) para transferência de payload sobre a conexão `ESTABLISHED`, seguida do suporte ao passive open (`listen()` e `accept()`).
