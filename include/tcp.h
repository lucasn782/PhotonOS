#ifndef PHOTONOS_TCP_H
#define PHOTONOS_TCP_H

#include <stddef.h>
#include <stdint.h>

#include "mutex.h"

/* RFC 793 minimum header; options may extend this. */
#define TCP_HEADER_MIN_SIZE       20U
#define TCP_DEFAULT_WINDOW        65535U
#define TCP_DEFAULT_MSS           1460U
#define TCP_MAX_BACKLOG           8U

/* IANA ephemeral / dynamic port range. */
#define TCP_EPHEMERAL_PORT_FIRST  49152U
#define TCP_EPHEMERAL_PORT_LAST   65535U

/* Control flags (header bitfield, host order after extraction). */
#define TCP_FLAG_FIN 0x01U
#define TCP_FLAG_SYN 0x02U
#define TCP_FLAG_RST 0x04U
#define TCP_FLAG_PSH 0x08U
#define TCP_FLAG_ACK 0x10U
#define TCP_FLAG_URG 0x20U

/* PCB flag bits (software bookkeeping, not wire flags). */
#define TCP_PCB_FLAG_BOUND        (1U << 0)
#define TCP_PCB_FLAG_PASSIVE      (1U << 1)
#define TCP_PCB_FLAG_ACTIVE       (1U << 2)
#define TCP_PCB_FLAG_ACCEPT_READY (1U << 3)
#define TCP_PCB_FLAG_TIMER_RTO    (1U << 4)
#define TCP_PCB_FLAG_TIMER_KEEP   (1U << 5)
#define TCP_PCB_FLAG_TIMER_DACK   (1U << 6)

/* Default timer budgets in kernel ticks (infrastructure; armed later). */
#define TCP_RTO_TICKS_DEFAULT     100ULL
#define TCP_KEEPALIVE_TICKS       72000ULL
#define TCP_DELAYED_ACK_TICKS     20ULL
#define TCP_CONNECT_TIMEOUT_TICKS 500ULL

struct socket;

struct __attribute__((packed)) tcp_header {
    uint16_t src_port;
    uint16_t dest_port;
    uint32_t seq_num;
    uint32_t ack_num;
    uint16_t data_offset_flags;
    uint16_t window_size;
    uint16_t checksum;
    uint16_t urgent_ptr;
};
typedef struct tcp_header tcp_header_t;

/*
 * Complete RFC 793 TCP state machine definition (Phase 2).
 */
enum tcp_state {
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
};
typedef enum tcp_state tcp_state_t;

/* Queued application payload (receive) or unacknowledged data (send). */
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

/*
 * Timer slots prepared for retransmission, keep-alive and delayed ACK.
 * Values are absolute kernel tick deadlines; 0 means disarmed.
 */
struct tcp_timers {
    uint64_t retransmission; /* RTO / retransmit deadline */
    uint64_t keepalive;      /* idle keep-alive probe */
    uint64_t delayed_ack;    /* delayed ACK deadline */
    uint64_t timeout;        /* generic connect / idle timeout */
    uint32_t rto_ticks;      /* current RTO budget */
    uint32_t retransmit_count;
};

/*
 * Protocol Control Block (Phase 1).
 *
 * Addresses: network byte order.
 * Ports and sequence numbers: host byte order.
 * The per-PCB mutex protects state, timers and both segment queues.
 * The global PCB list lock protects registration and port allocation.
 */
typedef struct tcp_pcb {
    uint32_t local_ip;
    uint32_t remote_ip;
    uint16_t local_port;
    uint16_t remote_port;

    uint32_t snd_una;
    uint32_t snd_nxt;

    uint32_t rcv_nxt;

    uint16_t snd_wnd;
    uint16_t rcv_wnd;

    uint8_t state;

    struct socket *socket;

    struct tcp_pcb *next;

    /* Extended Kernel bookkeeping & synchronization fields */
    uint32_t seq_number;
    uint32_t ack_number;
    uint32_t window;
    uint64_t retransmission_timer;

    struct tcp_queue receive_queue;
    struct tcp_queue send_queue;
    uint32_t flags;
    struct tcp_timers timers;

    /* Passive open / accept infrastructure. */
    int backlog;
    int accept_count;
    struct tcp_pcb *parent;
    struct tcp_pcb *accept_next;
    struct tcp_pcb *accept_head;
    struct tcp_pcb *accept_tail;

    mutex_t lock;
} tcp_pcb_t;

void tcp_init(void);

/* Phase 1 Management Functions */
tcp_pcb_t *tcp_alloc(void);
void tcp_free(tcp_pcb_t *pcb);
int tcp_register(tcp_pcb_t *pcb);
void tcp_unregister(tcp_pcb_t *pcb);
tcp_pcb_t *tcp_lookup(uint32_t local_ip, uint32_t remote_ip,
    uint16_t local_port, uint16_t remote_port);

struct tcp_pcb *tcp_socket_create(void *socket);
void tcp_socket_destroy(struct tcp_pcb *pcb);

int tcp_bind(struct tcp_pcb *pcb, uint32_t local_ip, uint16_t local_port);
uint16_t tcp_allocate_ephemeral_port(struct tcp_pcb *pcb, uint32_t local_ip);
void tcp_release_port(struct tcp_pcb *pcb);

int tcp_listen(struct tcp_pcb *pcb, int backlog);
struct tcp_pcb *tcp_accept(struct tcp_pcb *listener);

/* Header Serialization / Parsing (Phase 3) */
int tcp_serialize_header(const struct tcp_header *hdr, uint8_t *buffer, size_t buf_size);
int tcp_parse_header(const uint8_t *buffer, size_t len, struct tcp_header *hdr);

/* Checksum (Phase 4) */
uint16_t tcp_checksum(uint32_t src_ip, uint32_t dest_ip,
    const void *segment, size_t length);

/* Transmission & Reception (Phases 6 & 7) */
int tcp_send_segment(struct tcp_pcb *pcb, uint32_t seq, uint32_t ack, uint8_t flags,
    uint16_t window, const void *payload, size_t len);
int tcp_input(uint32_t src_ip, uint32_t dest_ip, const uint8_t *segment,
    size_t length);
int tcp_output(struct tcp_pcb *pcb, uint8_t flags, const void *payload,
    size_t payload_len);

int tcp_receive_read(struct tcp_pcb *pcb, uint8_t *buffer, size_t length);
size_t tcp_receive_available(struct tcp_pcb *pcb);

/* Arm / clear timer slots */
void tcp_timer_arm_rto(struct tcp_pcb *pcb, uint64_t now_ticks);
void tcp_timer_arm_keepalive(struct tcp_pcb *pcb, uint64_t now_ticks);
void tcp_timer_arm_delayed_ack(struct tcp_pcb *pcb, uint64_t now_ticks);
void tcp_timer_clear(struct tcp_pcb *pcb);

const char *tcp_state_name(enum tcp_state state);

/* Unit Tests (Phase 9) */
void tcp_run_tests(void);

#endif

