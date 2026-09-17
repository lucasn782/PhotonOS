# 🌐 PhotonOS — TCP Phase 2B.2A: Receive Path, RX Buffer & recv()

Este documento especifica a arquitetura, implementação e validação experimental da **TCP Phase 2B.2A** no PhotonOS.

> [!IMPORTANT]
> **Escopo da TCP Phase 2B.2A:**
> Esta etapa implementa estritamente o **caminho de recepção de dados (Receive Path)** em conexões TCP já estabelecidas (`ESTABLISHED`), incluindo alocação de **ring buffer circular limitado (`tcp_rx_buffer_t`)** por PCB, validação de sequence numbers (`rcv_nxt`), descarte e re-ACK de segmentos duplicados e fora de ordem, cálculo da janela anunciada (*advertised window*), syscall `recv()`, bloqueio e despertar cooperativo no escalonador, leituras parciais, semântica de EOF (FIN do peer) e RST.
> **NÃO** inclui ainda o caminho de transmissão de dados (`send()`), buffer TX, janela deslizante (*sliding window*) completa, controle de congestionamento, SACK, window scaling, HTTP ou TLS.

---

## 1. Visão Geral do Caminho de Recepção (RX)

O fluxo completo de recepção de dados implementado e comprovado no fio (*wire*) é:

```text
Peer / Host Client                              PhotonOS (Kernel & User)
       |                                                    |
       |  [PKT 1] TCP DATA (Seq = X, Len = N)              |
       |--------------------------------------------------->|
       |                                            e1000 RX interrupt
       |                                                    |
       |                                            net_poll_packets()
       |                                                    |
       |                                                IPv4 parser
       |                                                    |
       |                                                tcp_input()
       |                                                    |
       |                                            tcp_lookup_locked()
       |                                                    |
       |                                              ESTABLISHED PCB
       |                                                    |
       |                                            Sequence Validation
       |                                              (Seq == rcv_nxt)
       |                                                    |
       |                                            Copia para rx_buf
       |                                                    |
       |                                              rcv_nxt += N
       |                                                    |
       |                                           window = free_space
       |                                                    |
       |  [PKT 2] TCP ACK (Seq = Y, Ack = X + N,            |
       |                   Window = window)                 |
       |<---------------------------------------------------|
       |                                                    |
       |                                            tcp_socket_notify()
       |                                                    |
       |                                           Acorda task em recv()
       |                                                    |
       |                                            Ring 3: recv() retorna
       |                                            (copia dados para user)
```

---

## 2. Arquitetura do RX Ring Buffer (`tcp_rx_buffer_t`)

Cada PCB alocado no PhotonOS possui um buffer circular independente de tamanho fixo:

```c
typedef struct tcp_rx_buffer {
    uint8_t *data;      /* Ponteiro para memória alocada dinamicamente */
    size_t capacity;    /* Capacidade total: 8192 bytes (TCP_RX_BUFFER_CAPACITY) */
    size_t head;        /* Índice de leitura */
    size_t tail;        /* Índice de escrita */
    size_t used;        /* Bytes válidos armazenados */
} tcp_rx_buffer_t;
```

### 2.1. Propriedades e Ciclo de Vida
1. **Isolamento Total**: Cada PCB possui seu próprio buffer. Conexões distintas nunca compartilham buffers de recepção.
2. **Alocação e Liberação Seguras**:
   - Alocado em `tcp_alloc()` via `kmalloc(TCP_RX_BUFFER_CAPACITY)`.
   - Desalocado em `tcp_free()` e `tcp_socket_destroy()` via `kfree(pcb->rx_buf.data)`.
   - Limpeza protegida sob `pcb->lock`.
3. **Escrita Circular (`tcp_rx_buffer_write_locked`)**:
   - Escreve até o espaço livre disponível (`capacity - used`).
   - Se os dados ultrapassam o fim do array linear, faz o wrap-around para o índice 0.
   - Atualiza `tail` e incrementa `used`.
4. **Leitura Circular (`tcp_rx_buffer_read_locked`)**:
   - Lê até a quantidade solicitada ou a quantidade de bytes disponíveis.
   - Se a leitura ultrapassa o fim do array linear, faz o wrap-around.
   - Atualiza `head` e decrementa `used`.
5. **Prevenção de Buffer Overflow**:
   - A escrita nunca ultrapassa `capacity - used`. Segmentos excedentes são truncados ou descartados de forma controlada.

---

## 3. Validação de Sequência e Políticas RFC 793

O controle de recepção utiliza a aritmética de sequence numbers de 32 bits (RFC 1982) implementada através das macros:
- `TCP_SEQ_LT(a, b)`: `(int32_t)(a - b) < 0`
- `TCP_SEQ_LE(a, b)`: `(int32_t)(a - b) <= 0`
- `TCP_SEQ_GT(a, b)`: `(int32_t)(a - b) > 0`
- `TCP_SEQ_GE(a, b)`: `(int32_t)(a - b) >= 0`
- `TCP_SEQ_EQ(a, b)`: `a == b`

### 3.1. Segmentos Em Ordem (`sequence == pcb->rcv_nxt`)
- O payload é copiado diretamente para `pcb->rx_buf`.
- O ponteiro de sequência esperada avança: `pcb->rcv_nxt += written`.
- O número de acknowledgment é atualizado: `pcb->ack_number = pcb->rcv_nxt`.
- A janela anunciada é recalculada: `pcb->window = capacity - used`.
- Um ACK imediato é transmitido para o peer contendo o novo `rcv_nxt` e `window`.

### 3.2. Segmentos Duplicados (`sequence < pcb->rcv_nxt`)
- O payload é sumariamente descartado para não duplicar dados no buffer do usuário.
- O `rcv_nxt` permanece inalterado.
- Um ACK é retransmitido contendo o `rcv_nxt` atual para orientar o peer sobre o estado da conexão.

### 3.3. Segmentos Fora de Ordem (`sequence > pcb->rcv_nxt`)
- **Política adotada**: Conforme especificado na Fase 7 dos requisitos, o PhotonOS descarta pacotes fora de ordem e emite um ACK duplicado contendo o `rcv_nxt` esperado.
- Não há fila de reassembly parcial complexa nesta fase, garantindo determinismo e ausência de vazamentos de memória ou estouros de heap.

---

## 4. Syscall `recv()` e Bloqueio/Wakeup no Escalonador

### 4.1. Assinatura e Validações
```c
int recv(int fd, void *buf, size_t len, int flags);
```
O kernel executa checagens rigorosas:
- Validação de descritor: `fd >= 0 && fd < TASK_MAX_FDS`.
- Validação de ponteiro Ring 3: `vmm_validate_user_ptr(buf, len, 1)`. Ponteiros nulos, pertencentes ao espaço de kernel ou endereços não mapeados retornam `-1` imediatamente sem causar kernel panic.
- Validação de tamanho: `len == 0` retorna `0` com sucesso (conforme POSIX).
- Validação de tipo: descritor deve ser do tipo `SOCK_STREAM` e protocolo `IP_PROTO_TCP`.
- Validação de flags: apenas flag 0 é aceita nesta etapa.

### 4.2. Leituras Parciais
Se `len < rx_buf.used`, o `recv()` consome apenas `len` bytes, preservando os bytes restantes no buffer circular para chamadas subsequentes.

### 4.3. Semântica de Bloqueio e Proteção contra Lost Wakeups
Quando `rx_buf.used == 0` em uma conexão `TCP_ESTABLISHED`:
1. As interrupções são desabilitadas (`save_and_disable_interrupts()`).
2. A condição é checada sob `pcb->lock`: se o buffer continua vazio e o estado é `ESTABLISHED`, a task é colocada em `TASK_BLOCKED` com `wait_reason = TASK_WAIT_SOCKET_RECV` e `wait_target = (uint64_t)sock`.
3. O `pcb->lock` é liberado, as interrupções são restauradas e a CPU cede execução via `scheduler_yield()`.
4. Ao chegar um pacote de dados, a rotina `tcp_input()` executa `tcp_socket_notify(socket)` -> `scheduler_wake_socket()`, acordando a task bloqueada e inserindo-a de volta em `TASK_READY`.

### 4.4. Encerramento: EOF e RST
- **Peer FIN (EOF)**: Ao receber um segmento com flag FIN em ordem (`seq == rcv_nxt`), o kernel avança `rcv_nxt += 1`, transita para `TCP_CLOSE_WAIT`, marca `TCP_PCB_FLAG_EOF` e envia ACK. O `recv()` retorna os dados restantes que estavam no buffer; quando o buffer esvazia, chamadas subsequentes retornam `0` (EOF padrão POSIX).
- **RST**: Segmentos RST marcam `TCP_PCB_FLAG_RESET` e transitam o PCB para `TCP_CLOSED`. Tarefas bloqueadas são acordadas e `recv()` retorna `-1`.

---

## 5. Invariante de Concorrência: Transmissão sem Bloqueios

Conforme estabelecido nas Fases 2A e 2B.1:
> **Nenhuma transmissão de rede (`net_send_ipv4()`) pode ser executada enquanto locks críticos (`tcp_pcbs_lock` ou `pcb->lock`) estiverem mantidos.**

Todos os pacotes de ACK gerados pelo caminho de recepção têm seus cabeçalhos serializados sob `pcb->lock`, mas a chamada de rede é realizada **após** a liberação de todos os mutexes, eliminando completamente deadlocks recursivos com interrupções e filas de pacotes.

---

## 6. Resultados da Validação Experimental

### 6.1. Suíte de Testes da Phase 2B.2A (`scripts/test_tcp_phase2b2_rx.py`)
Executada no QEMU com port forwarding e captura PCAP:

| Teste | Descrição | Resultado |
|---|---|:---:|
| `UNIT_RING_BUFFER` | Escrita circular, leitura parcial e tracking de espaço no ring buffer | **PASS** |
| `UNIT_IN_ORDER` | Recepção in-order no kernel unit test e avanço de `rcv_nxt` | **PASS** |
| `UNIT_DUPLICATE` | Descarte de dados duplicados sem corromper buffer | **PASS** |
| `UNIT_OUT_OF_ORDER` | Descarte de pacotes fora de ordem por política | **PASS** |
| `UNIT_FIN_CLOSE_WAIT` | Transição para `CLOSE_WAIT` e marcação de EOF ao receber FIN | **PASS** |
| `RX_ERRORS` | Validação de fd inválido, NULL pointer, ponteiro inválido, len=0, UDP | **PASS** |
| `RX_SINGLE_BYTE` | Recepção de 1 byte isolado ("A") e ACK wire imediato | **PASS** |
| `RX_SMALL_PAYLOAD` | Recepção de string curta ("hello") | **PASS** |
| `RX_LARGE_PAYLOAD` | Recepção de bloco de 1024 bytes contínuos | **PASS** |
| `RX_PARTIAL_READ` | Leituras consecutivas de 4 + 4 + 2 bytes preservando integridade | **PASS** |
| `RX_BLOCKING_WAKE` | Bloqueio em `recv()` sem busy-wait e despertar por pacote de dados | **PASS** |
| `RX_MULTIPLE_CONNECTIONS` | Múltiplas conexões recebendo dados com 4-tuples e buffers isolados | **PASS** |
| `RX_FORK` | Processo filho herdando conexão e executando `recv()` | **PASS** |
| `RX_DUP` | Leitura de dados através de descritor duplicado via `dup()` | **PASS** |
| `PCAP_DATA_RECEIVED` | Confirmação de pacotes de dados capturados na interface de rede | **PASS** |
| `PCAP_ACK_TRANSMITTED` | Confirmação de pacotes de ACK emitidos pelo PhotonOS no fio | **PASS** |
| `PCAP_WINDOW_VALID` | Janela anunciada refletindo a capacidade livre do ring buffer | **PASS** |

### 6.2. Inspeção Wire PCAP (`logs/tcp_phase2b2_rx.pcap`)
Segmentos reais capturados no fio comprovando a resposta da stack:
```text
Host -> PhotonOS     | Seq=1088002 Ack=10819399 PayloadLen=6 (DATA "hello")
PhotonOS -> Host     | Seq=10819399 Ack=1088008 Window=8186 (ACK, window atualizada)
```

### 6.3. Testes de Regressão e Validação Estendida de RX
Todos os cenários essenciais e testes anteriores foram reexecutados e aprovados com 100% de estabilidade:

| Cenário de Validação | Escopo / Operação | Resultado |
|---|---|:---:|
| `RX_REPEATED` | Validação repetida de recepção contínua (5 iterações consecutivas) | **5/5 PASS** |
| `RECV_BLOCK` | Bloqueio cooperativo do processo em `recv()` sem dados e despertar imediato | **PASS** |
| `RECV_MULTI` | Recepção concorrente/consecutiva em múltiplas conexões ativas | **PASS** |
| `RECV_FORK` | Herança de socket conectado por processo filho via `fork()` executando `recv()` | **PASS** |
| `RECV_DUP` | Consumo de dados recebidos através de descritor duplicado via `dup()` | **PASS** |
| `test_tcp_phase2b_passive.py` | Passive Open, listen(), accept(), backlog, fork | **15/15 PASS** |
| `test_tcp_phase2a.py` | Active Open, connect(), SYN/ACK, RST, timeout | **PASS** |
| `test_10_boots.py` | Estabilidade de inicialização do sistema | **10/10 PASS** |
| `test_signals_suite.py` | Sinais POSIX, pipes, concorrência SMP | **PASS** |
| `test_vfs_qemu.py` | Operações de arquivos VFS e conectividade ICMP | **PASS** |

### 6.4. Conclusão Técnica e Métricas de Build
A conclusão correta da investigação técnica foi que a falha de runtime inicialmente suspeitada não se reproduziu quando o guest voltou a executar corretamente; o bloqueio inicial era a limitação de tamanho do kernel durante o build (`KERNEL_MAX_BYTES = 245760`).
- **Otimização de Compilação:** Flag `-Os` ativada em CFLAGS no `Makefile`.
- **Tamanho do Kernel Binário:** `build/photon.bin` = 137.772 bytes.
- **Limite Máximo (`KERNEL_MAX_BYTES`):** 245.760 bytes (480 setores LBA).
- **Margem de Segurança:** 107.988 bytes livres mantendo o gate de build ativo.
- **Limpeza de Instrumentação:** Todos os logs de depuração temporários (`[TCP DEBUG]`, `[PCB_LIFECYCLE]`, `debug_generation`, `[RECV_*]`, `[SLEEP_*]`, `[WAKE_*]`, `[NOTIFY_*]`) foram completamente eliminados do código-fonte.

---

## 7. Limitações Conhecidas e Próximos Passos

Esta etapa focou **exclusivamente no Receive Path**. As seguintes etapas subsequentes compõem a evolução da pilha TCP:
- **TCP Phase 2B.2B (Transmit Path)**: `send()`, alocação de buffer TX, enfileiramento de dados de saída, segmentação MSS 1460, retransmissão de dados não confirmados (RTO). (Concluída).
- **TCP Phase 2B.3**: Janela deslizante (*sliding window*) completa e controle de fluxo (`snd_wnd`).
- **TCP Phase 2C**: Máquina de estados completa de fechamento ativo e passivo (`FIN_WAIT_1`, `FIN_WAIT_2`, `TIME_WAIT`, `LAST_ACK`, `shutdown()`).
- **Recursos Futuros**: Controle de congestionamento (Slow Start, AIMD), SACK, Window Scaling, HTTP e TLS.
