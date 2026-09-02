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

## 🚀 8. Conclusão da Fase 2A & Preparação para a Fase 2B

A **Fase 2A** (Active Open / 3-Way Handshake) foi concluída e validada:
1. Implementação completa do Three-Way Handshake (`SYN -> SYN+ACK -> ACK`) com números de sequência monotônicos (`iss`), confirmação de `ack_num == iss + 1` e transição para `ESTABLISHED`.
2. Syscall `connect()` funcional com bloqueio cooperativo através do escalonador (`scheduler_sleep_current(TASK_WAIT_NETWORK)` e `scheduler_yield()`).
3. Temporizadores RTO com recuo exponencial e fila de transmissão diferida em `tcp_timer_tick()`.
4. Validação por captura PCAP no fio (*wire*) e testes de concorrência/estresse (`scripts/test_tcp_phase2a.py`).
Para detalhes completos, consulte [TCP Phase 2A](networking/tcp_phase2a.md).

A próxima etapa (**Fase 2B**) desenvolverá:
1. Transmissão e recepção contínua de dados (`send()`, `recv()`, `read()`, `write()`).
2. Abertura passiva no servidor (`listen()`, `accept()`, gerenciamento de fila de `backlog`).
3. Encerramento gracioso de quatro vias (`FIN`, `FIN+ACK`, `TIME_WAIT`).
