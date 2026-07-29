# 🧬 Protocol Control Block (PCB) — PhotonOS v4.2

## 🌐 Visão Geral

O **Protocol Control Block (PCB)**, definido pela estrutura `struct tcp_pcb` em `include/tcp.h`, é o coração do subsistema TCP do **PhotonOS v4.2**. Ele armazena todo o estado da conexão, endereçamento de 4 tuplas, números de sequência/reconhecimento, janela de recepção, filas de segmentos e temporizadores de retransmissão.

---

## 📐 Estrutura Completa do `struct tcp_pcb`

```c
struct tcp_pcb {
    /* Tupla de Demultiplexação de 4 elementos */
    uint32_t local_ip;              /* Endereço IPv4 local (Network Byte Order) */
    uint32_t remote_ip;             /* Endereço IPv4 remoto (Network Byte Order) */
    uint16_t local_port;            /* Porta TCP local (Host Byte Order) */
    uint16_t remote_port;           /* Porta TCP remota (Host Byte Order) */

    /* Variáveis do Estado do Protocolo */
    uint32_t seq_number;            /* Número de sequência de envio (SND.NXT) */
    uint32_t ack_number;            /* Número de reconhecimento de recepção (RCV.NXT) */
    uint32_t window;                /* Tamanho da janela anunciada */
    enum tcp_state state;           /* Estado da máquina de estados (CLOSED..ESTABLISHED) */

    /* Temporizadores (RFC 793 / RTO / Keep-Alive) */
    uint64_t retransmission_timer;  /* Legado: espelho de timers.retransmission */
    struct tcp_timers timers;       /* Estrutura de temporizadores (RTO, Keep-Alive, DACK) */

    /* Filas de Segmentos */
    struct tcp_queue receive_queue; /* Fila de recepção de dados da aplicação */
    struct tcp_queue send_queue;    /* Fila de envio pendente de ACK */
    uint32_t flags;                 /* Flags funcionais do PCB (BOUND, PASSIVE, ACTIVE, etc) */

    /* Infraestrutura de Passive Open (Listen/Accept) */
    int backlog;                    /* Capacidade máxima da fila de accept */
    int accept_count;               /* Quantidade atual de filhos prontos na fila de accept */
    struct tcp_pcb *parent;         /* Ponteiro para o PCB listener pai (se filho) */
    struct tcp_pcb *accept_next;    /* Próximo filho na fila de accept do listener */
    struct tcp_pcb *accept_head;    /* Cabeça da fila de conexões pendentes no listener */
    struct tcp_pcb *accept_tail;    /* Cauda da fila de conexões pendentes no listener */

    /* Associação VFS e Sincronização */
    void *socket;                   /* Ponteiro opaco para a struct socket correspondente */
    mutex_t lock;                   /* Mutex de exclusão mútua por PCB */
    struct tcp_pcb *next;           /* Próximo PCB na lista encadeada global */
};
```

---

## 🗂️ Filas de Segmentos (`struct tcp_queue` & `struct tcp_segment`)

Os dados em trânsito são organizados em filas de segmentos dinamicamente alocados via `kmalloc()`:

```c
struct tcp_segment {
    struct tcp_segment *next;
    uint32_t sequence;
    size_t length;
    size_t offset;
    uint8_t *data;
};

struct tcp_queue {
    struct tcp_segment *head;
    struct tcp_segment *tail;
    size_t bytes;
    size_t segments;
};
```

1. **Fila de Recepção (`receive_queue`)**: Recebe dados ordenados da rede. A aplicação consome esses dados chamando `tcp_receive_read()`.
2. **Fila de Envio (`send_queue`)**: Retém segmentos transmitidos que aguardam confirmação por ACK.

---

## ⏱️ Infraestrutura de Temporizadores (`struct tcp_timers`)

O PCB prepara a infraestrutura para suportar 4 temporizadores críticos de transporte:

```c
struct tcp_timers {
    uint64_t retransmission; /* Deadline de retransmissão RTO (kernel ticks) */
    uint64_t keepalive;      /* Probe de inatividade Keep-Alive */
    uint64_t delayed_ack;    /* Tempo limite de Delayed ACK */
    uint64_t timeout;        /* Timeout de conexão (Connect/Idle) */
    uint32_t rto_ticks;      /* Orçamento de RTO atual */
    uint32_t retransmit_count;
};
```

---

## 🔒 Gerenciamento da Lista Global & Sincronização

- Todos os PCBs ativos ficam encadeados na lista global `tcp_pcbs`.
- **`tcp_pcbs_lock`**: Mutex global que protege o registro, o desregistro e o algoritmo de busca (`tcp_lookup()`).
- **`pcb->lock`**: Mutex local em cada PCB para garantir operações thread-safe e preventivas em SMP durante inserções/remoções de dados nas filas e transições de estado.
