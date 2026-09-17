# 🏗️ Arquitetura do Subsistema TCP — PhotonOS v4.2

## 🌐 1. Visão Geral do Subsistema TCP

O subsistema TCP (Transmission Control Protocol) do **PhotonOS v4.2** foi projetado como um módulo kernel desacoplado, modular e thread-safe (`src/kernel/tcp.c`, `include/tcp.h`). Ele estende a pilha de rede nativa (Ethernet → ARP → IPv4 → ICMP → Socket Layer) preparando toda a infraestrutura de transporte com orientação a conexões.

---

## 📐 2. Diagrama Completo da Pilha de Rede

```text
+-------------------------------------------------------------------+
|                     Aplicações de Usuário (Ring 3)                |
|               chamadas POSIX: socket(), bind(), read(), write()   |
+-------------------------------------------------------------------+
                                  |
                                  v
+-------------------------------------------------------------------+
|                      Camada de Sockets & VFS                      |
|           nós VFS_NODE_SOCKET (`sys_socket`, `sys_bind`)          |
+-------------------------------------------------------------------+
                                  |
                                  v
+-------------------------------------------------------------------+
|                  Camada de Transporte TCP (L4)                    |
|  - Gerenciador de PCBs (tcp_pcbs global)                          |
|  - Máquina de Estados RFC 793 (10 estados)                        |
|  - Checksum TCP RFC 793 / RFC 1071 (Pseudo-Header IPv4)           |
|  - Parser e Serializador de Cabeçalho                             |
|  - Entradas: `tcp_input()` | Saídas: `tcp_send_segment()`          |
+-------------------------------------------------------------------+
                                  |
                                  v
+-------------------------------------------------------------------+
|                       Camada de Rede IPv4 (L3)                    |
|   - `net_send_ipv4()` (Protocolo IP_PROTO_TCP = 6)                 |
|   - Demux por protocolo em `net_poll_packets()`                   |
|   - Suporte mantido: ICMP (Echo Request/Reply), UDP, RAW          |
+-------------------------------------------------------------------+
                                  |
                                  v
+-------------------------------------------------------------------+
|                       Camada de Enlace (L2)                       |
|           - Resolução ARP (`arp_resolve` / `arp_cache`)           |
|           - Cabeçalho Ethernet II (ETH_TYPE_IPV4 = 0x0800)        |
+-------------------------------------------------------------------+
                                  |
                                  v
+-------------------------------------------------------------------+
|                      Driver Intel e1000 (Hardware)                |
|                    Transmissão e Recepção via DMA                 |
+-------------------------------------------------------------------+
```

---

## 🔀 3. Fluxo de Entrada e Saída de Segmentos

### 📥 Fluxo de Entrada (RX Segment Pipeline)

```text
Ethernet Frame (e1000 DMA)
  ↓
EtherType == 0x0800 (IPv4)
  ↓
IP Protocol == 6 (IP_PROTO_TCP)
  ↓
`tcp_input(src_ip, dest_ip, segment, length)`
  │
  ├── 1. Validação de Checksum (Pseudo-Header IPv4 + TCP Header + Payload)
  ├── 2. Desserialização do Cabeçalho (`tcp_parse_header()`)
  ├── 3. Instrumentação de Log `[TCP RX]`
  ├── 4. Demux / Lookup de PCB (`tcp_lookup()` 4-Tuple ou LISTEN)
  └── 5. Atualização de Sequência/Ack (`rcv_nxt`, `snd_una`, `rcv_wnd`) no PCB
```

### 📤 Fluxo de Saída (TX Segment Pipeline)

```text
`tcp_send_segment(pcb, seq, ack, flags, window, payload, len)`
  ↓
Montagem do Cabeçalho TCP (`tcp_serialize_header()`)
  ↓
Cálculo de Checksum TCP RFC 793 (`tcp_checksum()`)
  ↓
Instrumentação de Log `[TCP TX]`
  ↓
Atualização de `snd_nxt` no PCB
  ↓
`net_send_ipv4(remote_ip, IP_PROTO_TCP, buffer, total_len)`
  ↓
Encapsulamento Ethernet + Transmissão DMA e1000
```

---

## 🔬 4. Estrutura do PCB (Protocol Control Block)

A estrutura `tcp_pcb_t` (`struct tcp_pcb`) armazena todo o estado de uma conexão TCP local ou socket listener:

```c
typedef struct tcp_pcb {
    uint32_t local_ip;      /* IP local em ord. de rede */
    uint32_t remote_ip;     /* IP remoto em ord. de rede */

    uint16_t local_port;    /* Porta local em ord. de host */
    uint16_t remote_port;   /* Porta remota em ord. de host */

    uint32_t snd_una;       /* Unacknowledged Sequence */
    uint32_t snd_nxt;       /* Next Sequence to Send */

    uint32_t rcv_nxt;       /* Next Sequence Expected */

    uint16_t snd_wnd;       /* Send Window */
    uint16_t rcv_wnd;       /* Receive Window */

    uint8_t state;          /* Estado do protocolo (enum tcp_state) */

    struct socket *socket;  /* Ponteiro para o socket VFS associado */

    struct tcp_pcb *next;   /* Próximo nó na lista global de PCBs */

    /* Campos de sincronização e filas de kernel */
    uint32_t seq_number;
    uint32_t ack_number;
    uint32_t window;
    uint64_t retransmission_timer;

    struct tcp_queue receive_queue;
    struct tcp_queue send_queue;
    uint32_t flags;
    struct tcp_timers timers;

    /* Infraestrutura para Passive Open / Accept */
    int backlog;
    int accept_count;
    struct tcp_pcb *parent;
    struct tcp_pcb *accept_next;
    struct tcp_pcb *accept_head;
    struct tcp_pcb *accept_tail;

    mutex_t lock;
} tcp_pcb_t;
```

---

## ⚙️ 5. Gerenciador de Conexões e Ciclo de Vida

O gerenciamento de PCBs é coordenado por uma lista global (`tcp_pcbs`) com exclusão mútua garantida por `tcp_pcbs_lock`:

- **`tcp_alloc()`**: Aloca dinamicamente um novo PCB no Kernel Heap, inicializa estado para `TCP_CLOSED`, define janelas padrão (65535 bytes) e inicializa a mutex individual do PCB (`pcb->lock`). Emite log `[TCP] pcb criado`.
- **`tcp_free(pcb)`**: Remove o PCB da lista global via `tcp_unregister()`, limpa as filas de recepção/envio, redefine temporizadores e desaloca a memória física (`kfree`).
- **`tcp_register(pcb)`**: Insere o PCB de maneira segura na lista global `tcp_pcbs`.
- **`tcp_unregister(pcb)`**: Desconecta o PCB da lista encadeada global.
- **`tcp_lookup(local_ip, remote_ip, local_port, remote_port)`**: Efetua busca por correspondência exata de 4-tuple (`local_ip, remote_ip, local_port, remote_port`) e fallback para conexões passivas em estado `TCP_LISTEN`.

---

## 🔄 6. Máquina de Estados TCP (RFC 793)

O subsistema define formalmente os 10 estados padronizados da RFC 793:

```c
typedef enum tcp_state {
    TCP_CLOSED = 0,
    TCP_LISTEN,
    TCP_SYN_SENT,
    TCP_SYN_RECEIVED,
    TCP_ESTABLISHED,
    TCP_FIN_WAIT1,
    TCP_FIN_WAIT2,
    TCP_CLOSE_WAIT,
    TCP_LAST_ACK,
    TCP_TIME_WAIT
} tcp_state_t;
```

A função `tcp_state_name(state)` converte os valores numéricos em representações textuais legíveis para logs e auditoria.

---

## 🔌 7. Integração com o Socket Layer

Ao criar um socket através da chamada de sistema:

```c
int fd = sys_socket(AF_INET, SOCK_STREAM, IP_PROTO_TCP);
```

O Kernel aloca o socket e automaticamente invoca `tcp_alloc()` e `tcp_register()`, vinculando o PCB à estrutura do socket (`sock->tcp = pcb; pcb->socket = sock;`).

> [!NOTE]
> Os sockets do tipo `SOCK_RAW` e `SOCK_DGRAM` (UDP) e o protocolo ICMP continuam funcionando em suas rotas dedicadas sem qualquer alteração ou interferência.

---

## 🚀 8. Conclusão das Fases 2A, 2B (2B.1, 2B.2A, 2B.2B) & Próximos Passos

### 8.1. Fase 2A (Active Open / 3-Way Handshake) — Concluída
1. Implementação completa do Three-Way Handshake (`SYN -> SYN+ACK -> ACK`) com números de sequência monotônicos (`iss`), confirmação de `ack_num == iss + 1` e transição para `ESTABLISHED`.
2. Syscall `connect()` funcional com bloqueio cooperativo através do escalonador (`scheduler_sleep_current(TASK_WAIT_NETWORK)` e `scheduler_yield()`).
3. Temporizadores RTO com recuo exponencial e fila de transmissão diferida em `tcp_timer_tick()`.
4. Validação por captura PCAP no fio (*wire*) e testes de concorrência/estresse (`scripts/test_tcp_phase2a.py`).
Para detalhes completos, consulte [TCP Phase 2A](networking/tcp_phase2a.md).

### 8.2. Fase 2B.1 (Passive Open, Listen, Accept & Backlog) — Concluída
1. Implementação do lado servidor passivo: chamadas de sistema `listen()` e `accept()` com bloqueio cooperativo e despertar imediato no escalonador.
2. Separação rigorosa de ciclo de vida entre Listener PCB (`state = TCP_LISTEN`) e Child PCBs (`state = TCP_SYN_RECEIVED -> TCP_ESTABLISHED`).
3. Fila de backlog com enfileiramento após `ACK` final e saturação controlada por `TCP_MAX_BACKLOG` (16).
4. Demultiplexação prioritária: conexões ativas com tupla de 4 elementos exata sobrepõem listeners genéricos.
5. Validação de conexões simultâneas, consecutivas, isolamento de descritores pós-`fork()` e captura PCAP (`scripts/test_tcp_phase2b_passive.py`).
Para detalhes completos, consulte [TCP Phase 2B.1](networking/tcp_phase2b_passive.md).

### 8.3. Fase 2B.2A (Receive Path, RX Buffer & recv()) — Concluída
1. Buffer circular de recepção (`tcp_rx_buffer_t`) com 8192 bytes por PCB, sem busy-wait e com cálculo de advertised window.
2. Ingestão in-order de dados (`seq == rcv_nxt`), avanço monotônico de `rcv_nxt` e emissão de ACK.
3. Descarte de duplicados e fora de ordem com re-emissão de ACK atualizado.
4. Syscall `recv()` (SYS_RECV = 54) com suspensão cooperativa (`TASK_WAIT_SOCKET_RECV`), tratamento de EOF (`TCP_CLOSE_WAIT`) e RST.
5. Validação de `blocking recv`, `multiple receive`, `fork`, `dup` e repetição contínua (5/5).
Para detalhes completos, consulte [TCP Phase 2B.2A](networking/tcp_phase2b2_rx.md).

### 8.4. Fase 2B.2B (Transmit Path, TX Buffer, send() & Data ACK) — Concluída
1. Estrutura `tcp_tx_buffer_t` de 8192 bytes com separação entre `unsent` e `unacked` e cópia kernel-owned.
2. Syscall `send()` (SYS_SEND = 55) com semântica de escrita parcial não-bloqueante sob saturação de buffer.
3. Segmentação com `TCP_DEFAULT_MSS = 1460`, cálculo de checksum TCP com pseudo-header IPv4, avanço de `SND.NXT`.
4. Processamento de ACK cumulativo/parcial avançando `SND.UNA` e liberando segmentos confirmados.
5. Retransmissão RTO de dados com backoff exponencial e limite de tentativas (`TCP_MAX_DATA_RETRIES = 5`).
Para detalhes completos, consulte [TCP Phase 2B.2B](networking/tcp_phase2b2_tx.md).

### 8.5. Métricas de Build & Otimização do Kernel
- **Otimização de Compilação:** Flag `-Os` ativada em CFLAGS no `Makefile`.
- **Tamanho do Kernel Binário:** `build/photon.bin` = 137.772 bytes.
- **Limite Máximo do Kernel (`KERNEL_MAX_BYTES`):** 245.760 bytes (480 setores LBA × 512 bytes).
- **Margem de Segurança:** 107.988 bytes livres antes do teto de carregamento do bootloader.
- **Gate de Build Ativo:** `test $(stat -c%s build/photon.bin) -le 245760` no Makefile.

### 8.6. Próximas Etapas (Fase 2B.3 & Fase 2C):
1. **Fase 2B.3 (Controle de Janela & Fluxo):** Janela deslizante (*sliding window*) dinâmica com controle de créditos via `snd_wnd`.
2. **Fase 2C (Encerramento Gracioso & Full-Duplex):** Máquina de estados de fechamento ativo e passivo (`FIN_WAIT_1`, `FIN_WAIT_2`, `TIME_WAIT`, `LAST_ACK`, `shutdown()`).
3. **Fases Posteriores:** Algoritmos de controle de congestionamento (Slow Start, AIMD, Fast Retransmit), SACK, Window Scaling e Servidor HTTP Ring 3.
