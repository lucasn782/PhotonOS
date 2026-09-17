# 🌐 PhotonOS — TCP Phase 2B.1: Passive Open, Listen, Accept & Backlog

Este documento especifica a arquitetura, implementação e validação experimental da **TCP Phase 2B.1** no PhotonOS.

> [!IMPORTANT]
> **Escopo da TCP Phase 2B.1:**
> Esta etapa estabelece estritamente a arquitetura de **Passive Open**, chamadas de sistema `listen()`, `accept()`, fila de **backlog**, ciclo de vida independente de listeners e sockets conectados derivados, bloqueio e despertar cooperativo no escalonador, e isolamento de descritores pós-`fork()`.
> **NÃO** inclui ainda buffers RX/TX, janelas deslizantes (*sliding window*), controle de fluxo/congestionamento ou transferência de dados contínuos (`send()`/`recv()`).

---

## 1. Visão Geral e Objetivos

A Fase 2B.1 do subsistema TCP do PhotonOS implementa a capacidade do sistema operacional de operar como servidor TCP (*passive open*), recebendo ativamente conexões remotas iniciadas por clientes externos através da interface de rede (e1000/QEMU) em estrita conformidade com a RFC 793.

O fluxo estabelecido no fio (*wire*) e comprovado por inspeção de pacotes PCAP é:

```text
Host / External Client                          PhotonOS (Server)
         |                                              |
         |                                     socket(SOCK_STREAM)
         |                                              |
         |                                            bind()
         |                                              |
         |                                           listen()
         |                                              |
         |                                         [PCB: LISTEN]
         |                                              |
         |                                          accept()
         |                                              |
         |                                        (Task Bloqueada)
         |                                              |
         | [PKT 1] SYN (Seq = client_ISS, Ack = 0)     |
         |--------------------------------------------->|
         |                                      Cria Child PCB
         |                                      [SYN-RECEIVED]
         |                                              |
         | [PKT 2] SYN + ACK (Seq = child_ISS,          |
         |                    Ack = client_ISS + 1)     |
         |<---------------------------------------------|
         |                                              |
         | [PKT 3] ACK (Seq = client_ISS + 1,           |
         |              Ack = child_ISS + 1)            |
         |--------------------------------------------->|
         |                                      Child ESTABLISHED
         |                                      Insere no Backlog
         v                                      Acorda accept()
    ESTABLISHED                                         |
                                                Retorna novo fd
                                                [Accepted Socket]
```

O listener original permanece indefinidamente no estado `LISTEN`, pronto para receber novas conexões simultâneas ou consecutivas.

---

## 2. Arquitetura de Componentes

### 2.1. Separação Estrita: Listener PCB vs. Connection PCB
O subsistema TCP separa categoricamente a estrutura do **Listener** das conexões **Child**:

* **Listener PCB:**
  - `state = TCP_LISTEN`
  - `local_ip` vinculado ou `0` (`INADDR_ANY`)
  - `local_port` fixo
  - `remote_ip = 0`, `remote_port = 0` (não associado a nenhum peer único)
  - `backlog`: Capacidade máxima da fila de conexões pendentes (limitada por `TCP_MAX_BACKLOG = 16`)
  - `accept_head`, `accept_tail`, `accept_count`: Lista encadeada de conexões prontas no backlog.
  - Nunca transiciona para `SYN_RECEIVED` ou `ESTABLISHED`.

* **Child Connection PCB:**
  - Alocado dinamicamente ao receber um `SYN` direcionado à porta do listener.
  - Possui 4-tuple completo e independente: `local_ip`, `local_port`, `remote_ip`, `remote_port`.
  - Possui Initial Sequence Number (`ISS`) próprio, monotônico e descorrelacionado do listener.
  - Rastreia `irs = sequence`, `snd_una`, `snd_nxt`, `rcv_nxt`.
  - Possui temporizadores próprios de RTO (`timers.retransmission`) e conexão (`timers.timeout`).
  - Campo `parent`: Aponta para o listener de origem enquanto aguarda ser recolhido por `accept()`.

### 2.2. Algoritmo de Demultiplexação de Entrada (`tcp_lookup_locked`)
A demultiplexação em `src/kernel/tcp.c` prioriza conexões ativas sobre listeners genéricos:
1. **Prioridade Absoluta:** Busca por correspondência exata dos 4 elementos da tupla (`local_ip`, `local_port`, `remote_ip`, `remote_port`) onde `state != TCP_LISTEN`. Se existir uma conexão ativa/filha em `SYN_RECEIVED` ou `ESTABLISHED`, o segmento é entregue a ela imediatamente.
2. **Fallback para Listener Exato:** Se não houver correspondência exata de 4-tuple, busca por listener com `state == TCP_LISTEN`, porta local coincidente e IP local correspondente.
3. **Fallback para Listener Wildcard:** Se não houver listener exato de IP, seleciona o listener registrado com `local_ip == 0` (`INADDR_ANY`).
4. **Descarte Seguro:** Se nenhum PCB for encontrado, o segmento é descartado sem efeitos colaterais.

### 2.3. Gerenciamento do Backlog
* **Enfileiramento (`tcp_accept_enqueue_locked`):** Quando o `ACK` final do handshake é processado em `tcp_input()`, o child PCB transiciona para `TCP_ESTABLISHED` e é inserido na cauda (`accept_tail`) do listener, incrementando `accept_count`.
* **Saturação do Backlog:** Se `accept_count >= backlog`, novos segmentos `SYN` são descartados silenciosamente sem alocar novos PCBs órfãos, forçando o cliente a retransmitir e evitando esgotamento de memória.
* **Desenfileiramento (`tcp_accept_dequeue_locked`):** A chamada `accept()` remove o elemento da cabeça (`accept_head`), decrementa `accept_count` e desvincula `child->parent = 0`.
* **Fechamento do Listener com Backlog Pendente (`tcp_accept_queue_detach_locked`):** Se o listener for fechado antes que as conexões do backlog sejam aceitas, a fila é desanexada e todas as conexões filhas pendentes são destruídas com segurança, evitando *use-after-free* ou vazamento de PCBs.

---

## 3. Integração com a Camada de Sockets, VFS e Syscalls

### 3.1. Syscall `sys_listen(int fd, int backlog)` (SYS_LISTEN = 35)
1. Valida o descritor de arquivo `fd` e certifica-se de que é um nó do tipo `VFS_NODE_SOCKET`.
2. Verifica se o socket é `SOCK_STREAM` (`IPPROTO_TCP`).
3. Rejeita sockets não vinculados a uma porta (`local_port == 0`).
4. Aplica saturação (*clamping*) ao valor de backlog entre `1` e `TCP_MAX_BACKLOG` (16).
5. Ativa o PCB associado: `pcb->state = TCP_LISTEN`, `pcb->flags |= TCP_PCB_FLAG_PASSIVE`.

### 3.2. Syscall `sys_accept(int fd, struct sockaddr *addr, uint32_t *addrlen)` (SYS_ACCEPT = 36)
1. Valida o descritor e assegura que o socket chamador está no estado `TCP_LISTEN`.
2. Se a fila de aceitação estiver vazia (`accept_count == 0`):
   - Coloca a tarefa chamadora em espera cooperativa no escalonador via `scheduler_sleep_current(TASK_WAIT_NETWORK, (uint64_t)sock)`.
   - Executa `scheduler_yield()`, liberando a CPU para a thread de rede e demais processos (zero *busy-wait*).
   - Ao receber o `ACK` final e enfileirar o child, a thread de rede invoca `tcp_socket_notify(listener_socket)`, acordando o processo servidor.
3. Desenfileira o child PCB da fila de backlog.
4. Aloca uma nova estrutura `socket_t` e um novo nó `vfs_node_t` completamente inicializados.
5. Aloca um novo descritor de arquivo `new_fd` no processo corrente via `vfs_open_node`.
6. Popula os argumentos opcionais de usuário `addr` e `addrlen` com o endereço e porta do cliente remoto (`struct sockaddr_in`).
7. Retorna `new_fd` ao espaço de usuário. O listener original continua ativo em `fd`.

### 3.3. Ciclo de Vida Pós-`fork()`
Quando o processo servidor aceita uma conexão e executa `fork()`:
* A tabela de descritores do processo filho clona os ponteiros de descrição de arquivo (`file_description_t`).
* O filho pode fechar `listener_fd` e processar a conexão no `client_fd`.
* O pai pode fechar `client_fd` e continuar no laço de `accept()` sobre `listener_fd`.
* A destruição de um descritor não afeta o outro devido ao desacoplamento entre descritores e PCBs.

---

## 4. Evidência Experimental e Inspeção de Pacotes (PCAP)

A validação automatizada de ponta a ponta foi executada através do script `scripts/test_tcp_phase2b_passive.py`, utilizando clientes TCP no Host conectados à porta redirecionada `127.0.0.1:18088 -> 10.0.2.15:8088` e o binário de Ring 3 `tcptest.elf`.

### 4.1. Resumo dos Casos de Teste

| Test Case | Descrição | Resultado |
|-----------|-----------|-----------|
| `LISTEN_CREATE` | Criação de socket, bind na porta 8088 e transição para `LISTEN` | **PASS** |
| `PASSIVE_SYN` | Recepção de SYN do host cliente pelo listener | **PASS** |
| `SYN_RECEIVED` | Alocação do child PCB em `SYN_RECEIVED` mantendo listener em `LISTEN` | **PASS** |
| `SYN_ACK_WIRE` | Transmissão de SYN+ACK pelo child com ISS único no PCAP | **PASS** |
| `FINAL_ACK` | Recepção de ACK final, transição para `ESTABLISHED` e inserção no backlog | **PASS** |
| `ACCEPT` | Retorno com sucesso de novo descritor conectado | **PASS** |
| `ACCEPT_BLOCK_WAKE` | Bloqueio em accept vazio e despertar imediato na conclusão do handshake | **PASS** |
| `MULTIPLE_CONNECTIONS`| 3 conexões consecutivas aceitas com sucesso | **PASS** |
| `DISTINCT_4TUPLES` | Validação de que conexões possuem portas remotas independentes | **PASS** |
| `LISTENER_STAYS_ALIVE`| Listener permanece receptivo após encerramento de conexões filhas | **PASS** |
| `FORK_LIFECYCLE` | Processamento de conexão aceita em processo filho pós-`fork()` | **PASS** |
| `RST_INVALID_CLIENT` | Rejeição de endpoints inválidos e validação de erros de syscall | **PASS** |
| `BACKLOG` | Inserção correta na fila de conexões pendentes | **PASS** |
| `CLOSE_LISTENER` | Encerramento seguro do listener | **PASS** |
| `ESTABLISHED_CHILD` | Verificação do handshake completo de 3 vias no wire | **PASS** |

### 4.2. Traço Real dos Segmentos no Fio (Captura PCAP de Conexões Múltiplas)

```text
[WIRE PKT 1 - SYN]     10.0.2.2:39742 -> 10.0.2.15:8088 | Seq=1088001  Ack=0        Flags=0x02 (SYN)
[WIRE PKT 2 - SYN+ACK] 10.0.2.15:8088 -> 10.0.2.2:39742 | Seq=10677968 Ack=1088002  Flags=0x12 (SYN+ACK)
[WIRE PKT 3 - ACK]     10.0.2.2:39742 -> 10.0.2.15:8088 | Seq=1088002  Ack=10677969 Flags=0x10 (ACK)

[WIRE PKT 1 - SYN]     10.0.2.2:56472 -> 10.0.2.15:8088 | Seq=1792001  Ack=0        Flags=0x02 (SYN)
[WIRE PKT 2 - SYN+ACK] 10.0.2.15:8088 -> 10.0.2.2:56472 | Seq=15396643 Ack=1792002  Flags=0x12 (SYN+ACK)
[WIRE PKT 3 - ACK]     10.0.2.2:56472 -> 10.0.2.15:8088 | Seq=1792002  Ack=15396644 Flags=0x10 (ACK)

[WIRE PKT 1 - SYN]     10.0.2.2:56488 -> 10.0.2.15:8088 | Seq=1920001  Ack=0        Flags=0x02 (SYN)
[WIRE PKT 2 - SYN+ACK] 10.0.2.15:8088 -> 10.0.2.2:56488 | Seq=15436053 Ack=1920002  Flags=0x12 (SYN+ACK)
[WIRE PKT 3 - ACK]     10.0.2.2:56488 -> 10.0.2.15:8088 | Seq=1920002  Ack=15436054 Flags=0x10 (ACK)
```

* Cada conexão possui portas remotas distintas geradas pela pilha do Host (`39742`, `56472`, `56488`).
* O PhotonOS gerou números de sequência iniciais (`ISS`) únicos e estritamente monotônicos (`10677968`, `15396643`, `15436053`).
* O cálculo do acknowledgement pelo PhotonOS cumpriu estritamente `Ack = client_ISS + 1`.

---

## 5. Testes de Não-Regressão

A implementação da Phase 2B.1 passou por validação contra toda a base histórica do sistema:
* **TCP Phase 2B.1 Suite:** 15/15 verificações aprovadas com 100% de sucesso (`scripts/test_tcp_phase2b_passive.py`).
* **TCP Phase 2A Suite:** 5/5 testes aprovados e 34 segmentos PCAP validados no fio (`scripts/test_tcp_phase2a.py`).
* **Estresse de Inicialização (10/10 Boots):** 10 boots consecutivos com IDT, PMM, VMM, Heap, FAT16, TCP, SMP e Shell validados (`scripts/test_10_boots.py`).
* **POSIX Signals & Process Lifecycle:** 6/6 testes de sinais, pipes e concorrência SMP aprovados (`scripts/test_signals_suite.py`).
* **VFS & Armazenamento:** Montagem dinâmica, buffer cache e ICMP RAW socket aprovados (`scripts/test_vfs_qemu.py`).

---

## 6. Próximos Passos (TCP Phase 2B.2)

A infraestrutura do servidor TCP está consolidada e estável. A próxima etapa natural consiste em implementar a transferência contínua de dados:
* Buffers circulares de recepção (RX) e transmissão (TX) no PCB.
* Chamadas de sistema `sys_send()` e `sys_recv()` (e integração com `read()`/`write()` no VFS).
* Mecanismo de janela deslizante (*sliding window*) e controle de fluxo.
* Encerramento ordenado de conexão (`FIN`, `FIN+ACK`, `TIME_WAIT`).
