#include "tcp.h"

#include "heap.h"
#include "net.h"
#include "scheduler.h"
#include "serial.h"
#include "sys/socket.h"

/* Keep TCP control state out of the legacy VGA aperture crossed by .bss. */
static struct tcp_pcb *tcp_pcbs __attribute__((section(".network_state")));
static mutex_t tcp_pcbs_lock __attribute__((section(".network_state")));
static uint16_t tcp_next_ephemeral __attribute__((section(".network_state"))) =
    TCP_EPHEMERAL_PORT_FIRST;

struct deferred_tx_pkt {
    uint8_t  packet[TCP_HEADER_MIN_SIZE + TCP_DEFAULT_MSS];
    size_t   packet_len;
    uint32_t remote_ip;
};
static struct deferred_tx_pkt tcp_deferred_tx[4] __attribute__((section(".network_state")));
/* Serializes use of the fixed deferred packet storage after list locks drop. */
static mutex_t tcp_deferred_tx_lock __attribute__((section(".network_state")));

/* Socket layer wakes sleepers only after TCP unlocks. */
extern void tcp_socket_notify(void *socket);
extern volatile uint64_t kernel_ticks;

static void tcp_copy(void *destination, const void *source, size_t length)
{
    uint8_t *out = destination;
    const uint8_t *in = source;

    for (size_t i = 0; i < length; i++) {
        out[i] = in[i];
    }
}

static void tcp_zero(void *destination, size_t length)
{
    uint8_t *out = destination;

    for (size_t i = 0; i < length; i++) {
        out[i] = 0;
    }
}

static uint32_t tcp_checksum_add(uint32_t sum, const uint8_t *bytes,
    size_t length)
{
    while (length > 1U) {
        sum += ((uint16_t)bytes[0] << 8) | bytes[1];
        bytes += 2;
        length -= 2U;
    }
    if (length != 0U) {
        sum += (uint16_t)bytes[0] << 8;
    }
    return sum;
}

static uint16_t tcp_checksum_finish(uint32_t sum)
{
    while ((sum >> 16) != 0U) {
        sum = (sum & 0xFFFFU) + (sum >> 16);
    }
    return (uint16_t)~sum;
}


/*
 * Effective local IPv4 used in the pseudo-header.  A PCB bound to INADDR_ANY
 * still emits segments from the host address configured by the IPv4 stack.
 */
static uint32_t tcp_effective_local_ip(const struct tcp_pcb *pcb)
{
    if (pcb == 0 || pcb->local_ip == 0U) {
        return htonl(0x0A00020FU); /* NET_LOCAL_IPV4 10.0.2.15 */
    }
    return pcb->local_ip;
}

static void tcp_timers_reset(struct tcp_pcb *pcb)
{
    pcb->timers.retransmission = 0;
    pcb->timers.keepalive = 0;
    pcb->timers.delayed_ack = 0;
    pcb->timers.timeout = 0;
    pcb->timers.rto_ticks = TCP_RTO_TICKS_DEFAULT;
    pcb->timers.retransmit_count = 0;
    pcb->retransmission_timer = 0;
    pcb->flags &= ~(TCP_PCB_FLAG_TIMER_RTO | TCP_PCB_FLAG_TIMER_KEEP |
        TCP_PCB_FLAG_TIMER_DACK);
}

static int tcp_port_in_use_locked(const struct tcp_pcb *except,
    uint32_t local_ip, uint16_t local_port)
{
    if (local_port == 0U) {
        return 1;
    }
    for (struct tcp_pcb *pcb = tcp_pcbs; pcb != 0; pcb = pcb->next) {
        if (pcb == except || pcb->local_port != local_port) {
            continue;
        }
        /* Collision if either side is wildcard or both share the address. */
        if (pcb->local_ip == 0U || local_ip == 0U ||
            pcb->local_ip == local_ip) {
            return 1;
        }
    }
    return 0;
}

static struct tcp_pcb *tcp_lookup_locked(uint32_t local_ip, uint32_t remote_ip,
    uint16_t local_port, uint16_t remote_port)
{
    struct tcp_pcb *exact_listener = 0;
    struct tcp_pcb *wildcard_listener = 0;

    for (struct tcp_pcb *pcb = tcp_pcbs; pcb != 0; pcb = pcb->next) {
        if (pcb->local_port != local_port) {
            continue;
        }
        if (pcb->local_ip != 0U && pcb->local_ip != local_ip) {
            continue;
        }
        /* Exact 4-tuple match (active/established/syn-received connection) wins unconditionally. */
        if (pcb->remote_ip == remote_ip && pcb->remote_port == remote_port &&
            pcb->state != TCP_LISTEN) {
            return pcb;
        }
        /* Match listener */
        if (pcb->state == TCP_LISTEN && pcb->remote_ip == 0U &&
            pcb->remote_port == 0U) {
            if (pcb->local_ip == local_ip) {
                exact_listener = pcb;
            } else if (pcb->local_ip == 0U && wildcard_listener == 0) {
                wildcard_listener = pcb;
            }
        }
    }
    return exact_listener != 0 ? exact_listener : wildcard_listener;
}

static void tcp_queue_clear_locked(struct tcp_queue *queue)
{
    while (queue->head != 0) {
        struct tcp_segment *entry = queue->head;
        queue->head = entry->next;
        kfree(entry->data);
        kfree(entry);
    }
    queue->tail = 0;
    queue->bytes = 0;
    queue->segments = 0;
}

static void tcp_queue_append_locked(struct tcp_queue *queue,
    struct tcp_segment *entry)
{
    entry->next = 0;
    if (queue->tail != 0) {
        queue->tail->next = entry;
    } else {
        queue->head = entry;
    }
    queue->tail = entry;
    queue->bytes += entry->length;
    queue->segments++;
}

static struct tcp_segment *tcp_queue_take_head_locked(struct tcp_queue *queue)
{
    struct tcp_segment *entry = queue->head;

    if (entry == 0) {
        return 0;
    }
    queue->head = entry->next;
    if (queue->head == 0) {
        queue->tail = 0;
    }
    entry->next = 0;
    if (queue->bytes >= entry->length) {
        queue->bytes -= entry->length;
    } else {
        queue->bytes = 0;
    }
    if (queue->segments != 0) {
        queue->segments--;
    }
    return entry;
}

static size_t tcp_tx_buffer_used_locked(const struct tcp_pcb *pcb)
{
    return pcb->tx_buf.unsent.bytes + pcb->tx_buf.unacked.bytes;
}

/* Detach pending accepted children; caller destroys them without listener lock. */
static int tcp_accept_queue_detach_locked(struct tcp_pcb *listener,
    struct tcp_pcb **orphans, int max_orphans)
{
    struct tcp_pcb *child = listener->accept_head;
    int count = 0;

    listener->accept_head = 0;
    listener->accept_tail = 0;
    listener->accept_count = 0;
    while (child != 0) {
        struct tcp_pcb *next = child->accept_next;
        child->accept_next = 0;
        child->parent = 0;
        child->flags &= ~TCP_PCB_FLAG_ACCEPT_READY;
        child->state = TCP_CLOSED;
        if (count < max_orphans) {
            orphans[count++] = child;
        }
        child = next;
    }
    return count;
}

static int tcp_accept_enqueue_locked(struct tcp_pcb *listener,
    struct tcp_pcb *child)
{
    if (listener == 0 || child == 0) {
        return -1;
    }
    if (listener->accept_count >= listener->backlog) {
        return -1;
    }
    child->accept_next = 0;
    child->parent = listener;
    child->flags |= TCP_PCB_FLAG_ACCEPT_READY;
    if (listener->accept_tail != 0) {
        listener->accept_tail->accept_next = child;
    } else {
        listener->accept_head = child;
    }
    listener->accept_tail = child;
    listener->accept_count++;
    return 0;
}

static struct tcp_pcb *tcp_accept_dequeue_locked(struct tcp_pcb *listener)
{
    struct tcp_pcb *child;

    if (listener == 0 || listener->accept_head == 0) {
        return 0;
    }
    child = listener->accept_head;
    listener->accept_head = child->accept_next;
    if (listener->accept_head == 0) {
        listener->accept_tail = 0;
    }
    child->accept_next = 0;
    child->parent = 0;
    child->flags &= ~TCP_PCB_FLAG_ACCEPT_READY;
    if (listener->accept_count > 0) {
        listener->accept_count--;
    }
    return child;
}

static int tcp_build_output_locked(struct tcp_pcb *pcb, uint8_t flags,
    const void *payload, size_t payload_len, uint8_t *packet,
    size_t *packet_len, uint32_t *remote_ip)
{
    struct tcp_header *header = (struct tcp_header *)packet;
    uint32_t local_ip;

    if (pcb->local_port == 0U || pcb->remote_ip == 0U ||
        pcb->remote_port == 0U) {
        return -1;
    }
    local_ip = tcp_effective_local_ip(pcb);
    header->src_port = htons(pcb->local_port);
    header->dest_port = htons(pcb->remote_port);

    uint32_t current_seq = pcb->snd_nxt;
    if ((flags & TCP_FLAG_SYN) != 0U &&
        (pcb->state == TCP_SYN_SENT || pcb->state == TCP_SYN_RECEIVED)) {
        current_seq = pcb->iss;
    }

    header->seq_num = htonl(current_seq);
    header->ack_num = htonl(pcb->ack_number);
    header->data_offset_flags = htons((5U << 12) | flags);
    header->window_size = htons((uint16_t)(pcb->window & 0xFFFFU));
    header->checksum = 0;
    header->urgent_ptr = 0;
    if (payload_len != 0U) {
        tcp_copy(packet + sizeof(*header), payload, payload_len);
    }
    *packet_len = sizeof(*header) + payload_len;
    header->checksum = htons(tcp_checksum(local_ip, pcb->remote_ip,
        packet, *packet_len));
    /* RFC 768/793: transmitted checksum of 0 is represented as 0xffff. */
    if (header->checksum == 0U) {
        header->checksum = 0xFFFFU;
    }

    if ((flags & TCP_FLAG_SYN) != 0U) {
        pcb->snd_nxt = pcb->iss + 1U;
        pcb->snd_una = pcb->iss;
        pcb->seq_number = pcb->snd_nxt;
    } else if ((flags & TCP_FLAG_FIN) != 0U) {
        pcb->snd_nxt++;
        pcb->seq_number = pcb->snd_nxt;
    }
    pcb->snd_nxt += (uint32_t)payload_len;
    pcb->seq_number = pcb->snd_nxt;
    *remote_ip = pcb->remote_ip;
    return 0;
}

tcp_pcb_t *tcp_alloc(void)
{
    tcp_pcb_t *pcb = kmalloc(sizeof(*pcb));

    if (pcb == 0) {
        return 0;
    }
    tcp_zero(pcb, sizeof(*pcb));
    pcb->rx_buf.data = kmalloc(TCP_RX_BUFFER_CAPACITY);
    if (pcb->rx_buf.data == 0) {
        kfree(pcb);
        return 0;
    }
    tcp_zero(pcb->rx_buf.data, TCP_RX_BUFFER_CAPACITY);
    pcb->rx_buf.capacity = TCP_RX_BUFFER_CAPACITY;
    pcb->rx_buf.head = 0;
    pcb->rx_buf.tail = 0;
    pcb->rx_buf.used = 0;
    pcb->tx_buf.capacity = TCP_TX_BUFFER_CAPACITY;

    pcb->window = TCP_DEFAULT_WINDOW;
    pcb->snd_wnd = (uint16_t)TCP_DEFAULT_WINDOW;
    pcb->rcv_wnd = (uint16_t)TCP_DEFAULT_WINDOW;
    pcb->state = TCP_CLOSED;
    tcp_timers_reset(pcb);
    mutex_init(&pcb->lock);

    klog("[TCP] pcb criado\n");
    return pcb;
}

void tcp_free(tcp_pcb_t *pcb)
{
    if (pcb == 0) {
        return;
    }
    tcp_unregister(pcb);
    mutex_lock(&pcb->lock);
    tcp_queue_clear_locked(&pcb->receive_queue);
    tcp_queue_clear_locked(&pcb->tx_buf.unsent);
    tcp_queue_clear_locked(&pcb->tx_buf.unacked);
    if (pcb->rx_buf.data != 0) {
        kfree(pcb->rx_buf.data);
        pcb->rx_buf.data = 0;
    }
    pcb->rx_buf.capacity = 0;
    pcb->rx_buf.head = 0;
    pcb->rx_buf.tail = 0;
    pcb->rx_buf.used = 0;
    pcb->tx_buf.capacity = 0;
    tcp_timers_reset(pcb);
    pcb->socket = 0;
    pcb->state = TCP_CLOSED;
    mutex_unlock(&pcb->lock);
    kfree(pcb);
}

static struct tcp_pcb *tcp_pcb_alloc(void *socket)
{
    struct tcp_pcb *pcb = tcp_alloc();
    if (pcb == 0) {
        return 0;
    }
    pcb->socket = socket;
    return pcb;
}

const char *tcp_state_name(enum tcp_state state)
{
    switch (state) {
    case TCP_CLOSED:       return "CLOSED";
    case TCP_LISTEN:       return "LISTEN";
    case TCP_SYN_SENT:     return "SYN_SENT";
    case TCP_SYN_RECEIVED: return "SYN_RECEIVED";
    case TCP_ESTABLISHED:  return "ESTABLISHED";
    case TCP_FIN_WAIT1:    return "FIN_WAIT1";
    case TCP_FIN_WAIT2:    return "FIN_WAIT2";
    case TCP_CLOSE_WAIT:   return "CLOSE_WAIT";
    case TCP_LAST_ACK:     return "LAST_ACK";
    case TCP_TIME_WAIT:    return "TIME_WAIT";
    default:               return "UNKNOWN";
    }
}

void tcp_timer_arm_rto(struct tcp_pcb *pcb, uint64_t now_ticks)
{
    if (pcb == 0) {
        return;
    }
    mutex_lock(&pcb->lock);
    if (pcb->timers.rto_ticks == 0U) {
        pcb->timers.rto_ticks = TCP_RTO_TICKS_DEFAULT;
    }
    pcb->timers.retransmission = now_ticks + pcb->timers.rto_ticks;
    pcb->retransmission_timer = pcb->timers.retransmission;
    pcb->flags |= TCP_PCB_FLAG_TIMER_RTO;
    mutex_unlock(&pcb->lock);
}

void tcp_timer_arm_keepalive(struct tcp_pcb *pcb, uint64_t now_ticks)
{
    if (pcb == 0) {
        return;
    }
    mutex_lock(&pcb->lock);
    pcb->timers.keepalive = now_ticks + TCP_KEEPALIVE_TICKS;
    pcb->flags |= TCP_PCB_FLAG_TIMER_KEEP;
    mutex_unlock(&pcb->lock);
}

void tcp_timer_arm_delayed_ack(struct tcp_pcb *pcb, uint64_t now_ticks)
{
    if (pcb == 0) {
        return;
    }
    mutex_lock(&pcb->lock);
    pcb->timers.delayed_ack = now_ticks + TCP_DELAYED_ACK_TICKS;
    pcb->flags |= TCP_PCB_FLAG_TIMER_DACK;
    mutex_unlock(&pcb->lock);
}

void tcp_timer_clear(struct tcp_pcb *pcb)
{
    if (pcb == 0) {
        return;
    }
    mutex_lock(&pcb->lock);
    tcp_timers_reset(pcb);
    mutex_unlock(&pcb->lock);
}

void tcp_timer_tick(uint64_t now_ticks)
{
    /* Deferred TX queue: we must NOT call net_send_ipv4 while holding
     * tcp_pcbs_lock, because net_send_ipv4 → net_poll_packets → tcp_input
     * would try to re-acquire tcp_pcbs_lock → deadlock.
     * Uses static tcp_deferred_tx in .network_state to prevent kernel stack bloat. */
    int deferred_count = 0;

    /* The packet array is static to keep 8 KiB kernel stacks shallow. */
    mutex_lock(&tcp_deferred_tx_lock);

    mutex_lock(&tcp_pcbs_lock);
    for (struct tcp_pcb *pcb = tcp_pcbs; pcb != 0; pcb = pcb->next) {
        mutex_lock(&pcb->lock);
        if (pcb->state == TCP_SYN_SENT) {
            /* Check connection timeout */
            if (pcb->timers.timeout != 0 && now_ticks >= pcb->timers.timeout) {
                pcb->state = TCP_CLOSED;
                tcp_timers_reset(pcb);
                void *sock = pcb->socket;
                mutex_unlock(&pcb->lock);
                if (sock != 0) {
                    tcp_socket_notify(sock);
                }
                continue;
            }
            /* Check RTO retransmission */
            if ((pcb->flags & TCP_PCB_FLAG_TIMER_RTO) != 0 &&
                pcb->timers.retransmission != 0 &&
                now_ticks >= pcb->timers.retransmission) {
                if (pcb->timers.retransmit_count >= TCP_MAX_SYN_RETRIES) {
                    pcb->state = TCP_CLOSED;
                    tcp_timers_reset(pcb);
                    void *sock = pcb->socket;
                    mutex_unlock(&pcb->lock);
                    if (sock != 0) {
                        tcp_socket_notify(sock);
                    }
                    continue;
                }

                pcb->timers.retransmit_count++;
                pcb->timers.rto_ticks = (uint32_t)(pcb->timers.rto_ticks * 2U);
                if (pcb->timers.rto_ticks > 1000U) {
                    pcb->timers.rto_ticks = 1000U;
                }
                pcb->timers.retransmission = now_ticks + pcb->timers.rto_ticks;
                pcb->retransmission_timer = pcb->timers.retransmission;

                if (deferred_count < 4) {
                    tcp_deferred_tx[deferred_count].packet_len = 0;
                    tcp_deferred_tx[deferred_count].remote_ip = 0;
                    int res = tcp_build_output_locked(pcb, TCP_FLAG_SYN, 0, 0,
                        tcp_deferred_tx[deferred_count].packet,
                        &tcp_deferred_tx[deferred_count].packet_len,
                        &tcp_deferred_tx[deferred_count].remote_ip);
                    if (res == 0) {
                        deferred_count++;
                    }
                }
                mutex_unlock(&pcb->lock);
                continue;
            }
        } else if (pcb->state == TCP_SYN_RECEIVED) {
            /* Passive child timeout check */
            if (pcb->timers.timeout != 0 && now_ticks >= pcb->timers.timeout) {
                pcb->state = TCP_CLOSED;
                tcp_timers_reset(pcb);
                mutex_unlock(&pcb->lock);
                continue;
            }
            /* Passive child SYN+ACK RTO retransmission */
            if ((pcb->flags & TCP_PCB_FLAG_TIMER_RTO) != 0 &&
                pcb->timers.retransmission != 0 &&
                now_ticks >= pcb->timers.retransmission) {
                if (pcb->timers.retransmit_count >= TCP_MAX_SYN_RETRIES) {
                    pcb->state = TCP_CLOSED;
                    tcp_timers_reset(pcb);
                    mutex_unlock(&pcb->lock);
                    continue;
                }

                pcb->timers.retransmit_count++;
                pcb->timers.rto_ticks = (uint32_t)(pcb->timers.rto_ticks * 2U);
                if (pcb->timers.rto_ticks > 1000U) {
                    pcb->timers.rto_ticks = 1000U;
                }
                pcb->timers.retransmission = now_ticks + pcb->timers.rto_ticks;
                pcb->retransmission_timer = pcb->timers.retransmission;

                if (deferred_count < 4) {
                    tcp_deferred_tx[deferred_count].packet_len = 0;
                    tcp_deferred_tx[deferred_count].remote_ip = 0;
                    int res = tcp_build_output_locked(pcb,
                        (uint8_t)(TCP_FLAG_SYN | TCP_FLAG_ACK), 0, 0,
                        tcp_deferred_tx[deferred_count].packet,
                        &tcp_deferred_tx[deferred_count].packet_len,
                        &tcp_deferred_tx[deferred_count].remote_ip);
                    if (res == 0) {
                        deferred_count++;
                    }
                }
                mutex_unlock(&pcb->lock);
                continue;
            }
        } else if (pcb->state == TCP_ESTABLISHED) {
            /* Check data RTO retransmission */
            if ((pcb->flags & TCP_PCB_FLAG_TIMER_RTO) != 0 &&
                pcb->timers.retransmission != 0 &&
                now_ticks >= pcb->timers.retransmission) {
                if (pcb->tx_buf.unacked.head != 0) {
                    if (pcb->timers.retransmit_count >= TCP_MAX_DATA_RETRIES) {
                        /* Retransmission limit reached: reset connection */
                        pcb->state = TCP_CLOSED;
                        pcb->flags |= TCP_PCB_FLAG_RESET;
                        tcp_timers_reset(pcb);
                        tcp_queue_clear_locked(&pcb->tx_buf.unsent);
                        tcp_queue_clear_locked(&pcb->tx_buf.unacked);
                        void *sock = pcb->socket;
                        mutex_unlock(&pcb->lock);
                        if (sock != 0) {
                            tcp_socket_notify(sock);
                        }
                        continue;
                    }

                    /* Increment retry count & apply exponential backoff */
                    pcb->timers.retransmit_count++;
                    pcb->timers.rto_ticks = (uint32_t)(pcb->timers.rto_ticks * 2U);
                    if (pcb->timers.rto_ticks > 1000U) {
                        pcb->timers.rto_ticks = 1000U;
                    }
                    pcb->timers.retransmission = now_ticks + pcb->timers.rto_ticks;
                    pcb->retransmission_timer = pcb->timers.retransmission;

                    /* Retransmit oldest unacknowledged segment with original SEQ */
                    struct tcp_segment *seg = pcb->tx_buf.unacked.head;
                    if (seg != 0 && seg->data != 0 && deferred_count < 4) {
                        size_t seg_len = seg->length;
                        if (seg_len > TCP_DEFAULT_MSS) {
                            seg_len = TCP_DEFAULT_MSS;
                        }
                        uint32_t local_ip = tcp_effective_local_ip(pcb);
                        uint32_t remote_ip = pcb->remote_ip;
                        uint8_t *pkt = tcp_deferred_tx[deferred_count].packet;
                        struct tcp_header *hdr = (struct tcp_header *)pkt;

                        hdr->src_port = htons(pcb->local_port);
                        hdr->dest_port = htons(pcb->remote_port);
                        hdr->seq_num = htonl(seg->sequence);
                        hdr->ack_num = htonl(pcb->rcv_nxt);
                        hdr->data_offset_flags = htons((5U << 12) | TCP_FLAG_ACK | TCP_FLAG_PSH);
                        hdr->window_size = htons((uint16_t)(pcb->window & 0xFFFFU));
                        hdr->checksum = 0;
                        hdr->urgent_ptr = 0;
                        if (seg_len != 0) {
                            tcp_copy(pkt + sizeof(struct tcp_header), seg->data + seg->offset, seg_len);
                        }
                        size_t total_len = sizeof(struct tcp_header) + seg_len;
                        uint16_t csum = tcp_checksum(local_ip, remote_ip, pkt, total_len);
                        if (csum == 0U) {
                            csum = 0xFFFFU;
                        }
                        hdr->checksum = htons(csum);

                        tcp_deferred_tx[deferred_count].packet_len = total_len;
                        tcp_deferred_tx[deferred_count].remote_ip = remote_ip;
                        seg->sent_at = now_ticks;
                        seg->retransmit_count++;
                        deferred_count++;
                    }
                } else {
                    /* No unacknowledged data in queue, disarm RTO timer */
                    pcb->flags &= ~TCP_PCB_FLAG_TIMER_RTO;
                    pcb->timers.retransmission = 0;
                    pcb->retransmission_timer = 0;
                }
            }
        }
        mutex_unlock(&pcb->lock);
    }
    mutex_unlock(&tcp_pcbs_lock);

    /* Transmit deferred packets with NO locks held. */
    for (int i = 0; i < deferred_count; i++) {
        klog("[TCP TX] segmento retransmitido\n");
        net_send_ipv4(tcp_deferred_tx[i].remote_ip, IP_PROTO_TCP,
            tcp_deferred_tx[i].packet, tcp_deferred_tx[i].packet_len);
    }
    mutex_unlock(&tcp_deferred_tx_lock);
}

void tcp_init(void)
{
    tcp_pcbs = 0;
    tcp_next_ephemeral = TCP_EPHEMERAL_PORT_FIRST;
    mutex_init(&tcp_pcbs_lock);
    mutex_init(&tcp_deferred_tx_lock);
    klog("TCP: Subsistema PCB inicializado (fase 1).\n");
}

struct tcp_pcb *tcp_socket_create(void *socket)
{
    struct tcp_pcb *pcb = tcp_pcb_alloc(socket);

    if (pcb == 0) {
        return 0;
    }
    if (tcp_register(pcb) != 0) {
        tcp_free(pcb);
        return 0;
    }
    return pcb;
}

void tcp_socket_destroy(struct tcp_pcb *pcb)
{
    struct tcp_pcb *orphans[TCP_MAX_BACKLOG];
    int orphan_count = 0;
    struct tcp_pcb *syn_orphans[TCP_MAX_BACKLOG];
    int syn_orphan_count = 0;

    if (pcb == 0) {
        return;
    }



    /* If destroying a listener, identify half-open children */
    mutex_lock(&tcp_pcbs_lock);
    if (pcb->state == TCP_LISTEN) {
        for (struct tcp_pcb *c = tcp_pcbs; c != 0; c = c->next) {
            if (c->parent == pcb && c->state == TCP_SYN_RECEIVED &&
                syn_orphan_count < (int)TCP_MAX_BACKLOG) {
                syn_orphans[syn_orphan_count++] = c;
                c->parent = 0;
            }
        }
    }
    mutex_unlock(&tcp_pcbs_lock);

    tcp_unregister(pcb);
    mutex_lock(&pcb->lock);
    if (pcb->state == TCP_LISTEN) {
        orphan_count = tcp_accept_queue_detach_locked(pcb, orphans,
            (int)TCP_MAX_BACKLOG);
    }
    tcp_queue_clear_locked(&pcb->receive_queue);
    tcp_queue_clear_locked(&pcb->tx_buf.unsent);
    tcp_queue_clear_locked(&pcb->tx_buf.unacked);
    if (pcb->rx_buf.data != 0) {
        kfree(pcb->rx_buf.data);
        pcb->rx_buf.data = 0;
    }
    pcb->rx_buf.capacity = 0;
    pcb->rx_buf.head = 0;
    pcb->rx_buf.tail = 0;
    pcb->rx_buf.used = 0;
    pcb->tx_buf.capacity = 0;
    tcp_timers_reset(pcb);
    pcb->socket = 0;
    pcb->parent = 0;
    pcb->state = TCP_CLOSED;
    mutex_unlock(&pcb->lock);

    for (int i = 0; i < orphan_count; i++) {
        tcp_socket_destroy(orphans[i]);
    }
    for (int i = 0; i < syn_orphan_count; i++) {
        tcp_socket_destroy(syn_orphans[i]);
    }
    kfree(pcb);
}

int tcp_register(struct tcp_pcb *pcb)
{
    if (pcb == 0) {
        return -1;
    }

    mutex_lock(&tcp_pcbs_lock);
    for (struct tcp_pcb *entry = tcp_pcbs; entry != 0; entry = entry->next) {
        if (entry == pcb) {
            mutex_unlock(&tcp_pcbs_lock);
            return -1;
        }
    }
    pcb->next = tcp_pcbs;
    tcp_pcbs = pcb;
    mutex_unlock(&tcp_pcbs_lock);
    return 0;
}

void tcp_unregister(struct tcp_pcb *pcb)
{
    struct tcp_pcb **entry;

    if (pcb == 0) {
        return;
    }

    mutex_lock(&tcp_pcbs_lock);
    entry = &tcp_pcbs;
    while (*entry != 0) {
        if (*entry == pcb) {
            *entry = pcb->next;
            pcb->next = 0;
            break;
        }
        entry = &(*entry)->next;
    }
    mutex_unlock(&tcp_pcbs_lock);
}

struct tcp_pcb *tcp_lookup(uint32_t local_ip, uint32_t remote_ip,
    uint16_t local_port, uint16_t remote_port)
{
    struct tcp_pcb *pcb;

    mutex_lock(&tcp_pcbs_lock);
    pcb = tcp_lookup_locked(local_ip, remote_ip, local_port, remote_port);
    mutex_unlock(&tcp_pcbs_lock);
    return pcb;
}

int tcp_bind(struct tcp_pcb *pcb, uint32_t local_ip, uint16_t local_port)
{
    if (pcb == 0) {
        return -1;
    }
    /* Port 0 = request kernel to pick an ephemeral port. */
    if (local_port == 0U) {
        uint16_t assigned = tcp_allocate_ephemeral_port(pcb, local_ip);
        return (assigned != 0U) ? 0 : -1;
    }
    mutex_lock(&tcp_pcbs_lock);
    mutex_lock(&pcb->lock);
    if (pcb->state != TCP_CLOSED ||
        tcp_port_in_use_locked(pcb, local_ip, local_port)) {
        mutex_unlock(&pcb->lock);
        mutex_unlock(&tcp_pcbs_lock);
        return -1;
    }
    pcb->local_ip = local_ip;
    pcb->local_port = local_port;
    pcb->flags |= TCP_PCB_FLAG_BOUND;
    mutex_unlock(&pcb->lock);
    mutex_unlock(&tcp_pcbs_lock);
    return 0;
}

uint16_t tcp_allocate_ephemeral_port(struct tcp_pcb *pcb, uint32_t local_ip)
{
    uint16_t selected = 0;
    const uint32_t range =
        (uint32_t)TCP_EPHEMERAL_PORT_LAST - TCP_EPHEMERAL_PORT_FIRST + 1U;

    if (pcb == 0) {
        return 0;
    }
    mutex_lock(&tcp_pcbs_lock);
    mutex_lock(&pcb->lock);
    /* Only allow ephemeral bind when the PCB is still closed. */
    if (pcb->state != TCP_CLOSED) {
        mutex_unlock(&pcb->lock);
        mutex_unlock(&tcp_pcbs_lock);
        return 0;
    }
    for (uint32_t attempts = 0; attempts < range; attempts++) {
        uint16_t candidate;

        /* Removed dead comparison: > LAST is always false for uint16_t */
        if (tcp_next_ephemeral < TCP_EPHEMERAL_PORT_FIRST) {
            tcp_next_ephemeral = TCP_EPHEMERAL_PORT_FIRST;
        }
        candidate = tcp_next_ephemeral;
        if (tcp_next_ephemeral == TCP_EPHEMERAL_PORT_LAST) {
            tcp_next_ephemeral = TCP_EPHEMERAL_PORT_FIRST;
        } else {
            tcp_next_ephemeral++;
        }
        if (!tcp_port_in_use_locked(pcb, local_ip, candidate)) {
            pcb->local_ip = local_ip;
            pcb->local_port = candidate;
            pcb->flags |= TCP_PCB_FLAG_BOUND;
            selected = candidate;
            break;
        }
    }
    mutex_unlock(&pcb->lock);
    mutex_unlock(&tcp_pcbs_lock);
    return selected;
}

void tcp_release_port(struct tcp_pcb *pcb)
{
    if (pcb == 0) {
        return;
    }
    mutex_lock(&tcp_pcbs_lock);
    mutex_lock(&pcb->lock);
    pcb->local_port = 0;
    pcb->local_ip = 0;
    pcb->flags &= ~TCP_PCB_FLAG_BOUND;
    mutex_unlock(&pcb->lock);
    mutex_unlock(&tcp_pcbs_lock);
}

int tcp_listen(struct tcp_pcb *pcb, int backlog)
{
    if (pcb == 0 || pcb->local_port == 0U) {
        return -1;
    }
    if (backlog <= 0) {
        backlog = 1;
    }
    if (backlog > (int)TCP_MAX_BACKLOG) {
        backlog = (int)TCP_MAX_BACKLOG;
    }

    mutex_lock(&pcb->lock);
    if (pcb->state != TCP_CLOSED && pcb->state != TCP_LISTEN) {
        mutex_unlock(&pcb->lock);
        return -1;
    }
    pcb->state = TCP_LISTEN;
    pcb->remote_ip = 0;
    pcb->remote_port = 0;
    pcb->backlog = backlog;
    pcb->flags |= TCP_PCB_FLAG_PASSIVE | TCP_PCB_FLAG_BOUND;
    pcb->flags &= ~TCP_PCB_FLAG_ACTIVE;
    mutex_unlock(&pcb->lock);
    return 0;
}

struct tcp_pcb *tcp_accept(struct tcp_pcb *listener)
{
    struct tcp_pcb *child;

    if (listener == 0) {
        return 0;
    }
    mutex_lock(&listener->lock);
    if (listener->state != TCP_LISTEN) {
        mutex_unlock(&listener->lock);
        return 0;
    }
    child = tcp_accept_dequeue_locked(listener);
    mutex_unlock(&listener->lock);
    return child;
}

struct tcp_pcb *tcp_accept_locked(struct tcp_pcb *listener)
{
    if (listener == 0 || listener->state != TCP_LISTEN) {
        return 0;
    }
    return tcp_accept_dequeue_locked(listener);
}

int tcp_serialize_header(const struct tcp_header *hdr, uint8_t *buffer, size_t buf_size)
{
    if (hdr == 0 || buffer == 0 || buf_size < sizeof(struct tcp_header)) {
        return -1;
    }
    struct tcp_header *dest = (struct tcp_header *)buffer;
    dest->src_port = htons(hdr->src_port);
    dest->dest_port = htons(hdr->dest_port);
    dest->seq_num = htonl(hdr->seq_num);
    dest->ack_num = htonl(hdr->ack_num);
    dest->data_offset_flags = htons(hdr->data_offset_flags);
    dest->window_size = htons(hdr->window_size);
    dest->checksum = htons(hdr->checksum);
    dest->urgent_ptr = htons(hdr->urgent_ptr);
    return (int)sizeof(struct tcp_header);
}

int tcp_parse_header(const uint8_t *buffer, size_t len, struct tcp_header *hdr)
{
    if (buffer == 0 || hdr == 0 || len < sizeof(struct tcp_header)) {
        return -1;
    }
    const struct tcp_header *src = (const struct tcp_header *)buffer;
    hdr->src_port = ntohs(src->src_port);
    hdr->dest_port = ntohs(src->dest_port);
    hdr->seq_num = ntohl(src->seq_num);
    hdr->ack_num = ntohl(src->ack_num);
    hdr->data_offset_flags = ntohs(src->data_offset_flags);
    hdr->window_size = ntohs(src->window_size);
    hdr->checksum = ntohs(src->checksum);
    hdr->urgent_ptr = ntohs(src->urgent_ptr);
    return 0;
}

uint16_t tcp_checksum(uint32_t src_ip, uint32_t dest_ip,
    const void *segment, size_t length)
{
    const uint8_t *src = (const uint8_t *)&src_ip;
    const uint8_t *dest = (const uint8_t *)&dest_ip;
    uint32_t sum = 0;

    if (segment == 0 || length > 0xFFFFU) {
        return 0;
    }
    /* Pseudo-header: src, dest, zero||proto, TCP length. */
    sum = tcp_checksum_add(sum, src, 4U);
    sum = tcp_checksum_add(sum, dest, 4U);
    sum += IP_PROTO_TCP;
    sum += (uint16_t)length;
    sum = tcp_checksum_add(sum, segment, length);
    return tcp_checksum_finish(sum);
}

int tcp_send_segment(struct tcp_pcb *pcb, uint32_t seq, uint32_t ack, uint8_t flags,
    uint16_t window, const void *payload, size_t len)
{
    if (pcb == 0 || pcb->remote_ip == 0U || pcb->local_port == 0U || pcb->remote_port == 0U) {
        return -1;
    }
    uint8_t buffer[TCP_HEADER_MIN_SIZE + TCP_DEFAULT_MSS];
    /* Application data must pass through tcp_send() so it is retained for ACK/RTO. */
    if (len != 0U || payload != 0 || len > TCP_DEFAULT_MSS) {
        return -1;
    }

    struct tcp_header hdr;
    hdr.src_port = pcb->local_port;
    hdr.dest_port = pcb->remote_port;
    hdr.seq_num = seq;
    hdr.ack_num = ack;
    hdr.data_offset_flags = (uint16_t)((5U << 12) | (flags & 0x3FU));
    hdr.window_size = window != 0U ? window : pcb->snd_wnd;
    hdr.checksum = 0;
    hdr.urgent_ptr = 0;

    if (tcp_serialize_header(&hdr, buffer, sizeof(buffer)) < 0) {
        return -1;
    }

    if (len != 0U && payload != 0) {
        tcp_copy(buffer + sizeof(struct tcp_header), payload, len);
    }

    size_t total_len = sizeof(struct tcp_header) + len;
    uint32_t local_ip = tcp_effective_local_ip(pcb);
    uint16_t csum = tcp_checksum(local_ip, pcb->remote_ip, buffer, total_len);
    if (csum == 0U) {
        csum = 0xFFFFU;
    }

    struct tcp_header *raw_hdr = (struct tcp_header *)buffer;
    raw_hdr->checksum = htons(csum);

    klog("[TCP TX] segmento transmitido\n");

    return net_send_ipv4(pcb->remote_ip, IP_PROTO_TCP, buffer, total_len);
}

int tcp_output(struct tcp_pcb *pcb, uint8_t flags, const void *payload,
    size_t payload_len)
{
    uint8_t packet[TCP_HEADER_MIN_SIZE + TCP_DEFAULT_MSS];
    uint32_t remote_ip;
    size_t packet_len;

    if (pcb == 0 || payload_len > TCP_DEFAULT_MSS ||
        (payload_len != 0U && payload == 0)) {
        return -1;
    }
    if (payload_len != 0U) {
        int accepted = tcp_send(pcb, (const uint8_t *)payload, payload_len);
        return accepted == (int)payload_len ? 0 : -1;
    }
    mutex_lock(&pcb->lock);
    if (payload_len != 0U && pcb->state != TCP_ESTABLISHED) {
        mutex_unlock(&pcb->lock);
        return -1;
    }
    int result = tcp_build_output_locked(pcb, flags, payload, payload_len,
        packet, &packet_len, &remote_ip);
    if (result == 0 && (flags & TCP_FLAG_SYN) != 0U) {
        if (pcb->timers.rto_ticks == 0U) {
            pcb->timers.rto_ticks = TCP_RTO_TICKS_DEFAULT;
        }
        pcb->timers.retransmission = kernel_ticks + pcb->timers.rto_ticks;
        pcb->retransmission_timer = pcb->timers.retransmission;
        pcb->flags |= TCP_PCB_FLAG_TIMER_RTO;
    }
    mutex_unlock(&pcb->lock);
    return result == 0 ? net_send_ipv4(remote_ip, IP_PROTO_TCP, packet,
        packet_len) : -1;
}

/*
 * Passive open: LISTEN + SYN creates a child PCB in SYN_RECEIVED and replies
 * with SYN+ACK.  Full option negotiation and simultaneous open remain Phase 2.
 */
static int tcp_handle_listen_syn(struct tcp_pcb *listener, uint32_t src_ip,
    uint32_t dest_ip, uint16_t remote_port, uint32_t sequence)
{
    struct tcp_pcb *child;
    uint8_t packet[TCP_HEADER_MIN_SIZE];
    size_t packet_len = 0;
    uint32_t remote_ip = 0;
    int result;

    mutex_lock(&listener->lock);
    if (listener->state != TCP_LISTEN) {
        mutex_unlock(&listener->lock);
        return -1;
    }
    if (listener->accept_count >= listener->backlog) {
        mutex_unlock(&listener->lock);
        klog("TCP: Backlog do listener cheio, descartando SYN.\n");
        return -1;
    }
    uint32_t local_ip = listener->local_ip != 0U ? listener->local_ip : dest_ip;
    uint16_t local_port = listener->local_port;
    mutex_unlock(&listener->lock);

    child = tcp_pcb_alloc(0);
    if (child == 0) {
        return -1;
    }
    child->local_ip = local_ip;
    child->remote_ip = src_ip;
    child->local_port = local_port;
    child->remote_port = remote_port;

    /* Generate unique Initial Sequence Number (ISS) for child connection */
    uint32_t iss = (uint32_t)(kernel_ticks + 1000ULL + ((uint32_t)remote_port << 8));
    if (iss == 0U) {
        iss = 1000U;
    }
    child->iss = iss;
    child->irs = sequence;
    child->seq_number = iss;
    child->snd_una = iss;
    child->snd_nxt = iss + 1U;
    child->ack_number = sequence + 1U;
    child->rcv_nxt = sequence + 1U;
    child->window = TCP_DEFAULT_WINDOW;
    child->snd_wnd = (uint16_t)TCP_DEFAULT_WINDOW;
    child->rcv_wnd = (uint16_t)TCP_DEFAULT_WINDOW;
    child->state = TCP_SYN_RECEIVED;
    child->parent = listener;
    child->flags = TCP_PCB_FLAG_BOUND | TCP_PCB_FLAG_PASSIVE | TCP_PCB_FLAG_TIMER_RTO;
    child->timers.rto_ticks = (uint32_t)TCP_RTO_TICKS_DEFAULT;
    child->timers.retransmit_count = 0;
    child->timers.retransmission = kernel_ticks + child->timers.rto_ticks;
    child->timers.timeout = kernel_ticks + TCP_CONNECT_TIMEOUT_TICKS;
    child->retransmission_timer = child->timers.retransmission;

    if (tcp_register(child) != 0) {
        tcp_free(child);
        return -1;
    }

    mutex_lock(&child->lock);
    result = tcp_build_output_locked(child,
        (uint8_t)(TCP_FLAG_SYN | TCP_FLAG_ACK), 0, 0, packet, &packet_len,
        &remote_ip);
    mutex_unlock(&child->lock);

    if (result != 0) {
        tcp_socket_destroy(child);
        return -1;
    }

    (void)net_send_ipv4(remote_ip, IP_PROTO_TCP, packet, packet_len);
    klog("[TCP TX] SYN+ACK transmitido do child PCB\n");
    return 0;
}

int tcp_input(uint32_t src_ip, uint32_t dest_ip, const uint8_t *segment,
    size_t length)
{
    struct tcp_header hdr;
    struct tcp_pcb *pcb;
    uint16_t header_len;
    uint16_t flags;
    uint16_t local_port;
    uint16_t remote_port;
    uint32_t sequence;
    uint32_t acknowledgement;
    uint16_t raw_checksum;
    const uint8_t *payload;
    size_t payload_len;
    uint8_t out_packet[TCP_HEADER_MIN_SIZE];
    size_t out_packet_len = 0;
    uint32_t out_remote_ip = 0;
    int send_out = 0;
    void *socket = 0;
    void *listener_socket = 0;

    if (segment == 0 || length < sizeof(struct tcp_header)) {
        return -1;
    }

    raw_checksum = ntohs(((const struct tcp_header *)segment)->checksum);
    (void)raw_checksum;

    if (tcp_checksum(src_ip, dest_ip, segment, length) != 0U) {
        klog("TCP: Checksum invalido no segmento recebido.\n");
        return -1;
    }

    if (tcp_parse_header(segment, length, &hdr) != 0) {
        return -1;
    }

    header_len = (uint16_t)((hdr.data_offset_flags >> 12) * 4U);
    if (header_len < TCP_HEADER_MIN_SIZE || header_len > length) {
        return -1;
    }
    local_port = hdr.dest_port;
    remote_port = hdr.src_port;
    flags = hdr.data_offset_flags & 0x01FFU;
    sequence = hdr.seq_num;
    acknowledgement = hdr.ack_num;
    payload = segment + header_len;
    payload_len = length - header_len;

    klog("[TCP RX] segmento recebido\n");

    mutex_lock(&tcp_pcbs_lock);
    pcb = tcp_lookup_locked(dest_ip, src_ip, local_port, remote_port);
    if (pcb == 0) {
        mutex_unlock(&tcp_pcbs_lock);
        return -1;
    }

    mutex_lock(&pcb->lock);
    mutex_unlock(&tcp_pcbs_lock);

    pcb->snd_wnd = hdr.window_size;

    /* Passive open: demux landed on a LISTEN PCB. */
    if (pcb->state == TCP_LISTEN) {
        int listen_ok = 0;
        if ((flags & TCP_FLAG_SYN) != 0U && (flags & TCP_FLAG_ACK) == 0U &&
            (flags & TCP_FLAG_RST) == 0U) {
            listen_ok = 1;
        }
        mutex_unlock(&pcb->lock);
        if (!listen_ok) {
            return -1;
        }
        return tcp_handle_listen_syn(pcb, src_ip, dest_ip, remote_port,
            sequence);
    }

    if ((flags & TCP_FLAG_RST) != 0U) {
        int half_open_child = (pcb->parent != 0 && pcb->socket == 0);
        pcb->state = TCP_CLOSED;
        pcb->flags |= TCP_PCB_FLAG_RESET;
        tcp_queue_clear_locked(&pcb->tx_buf.unsent);
        tcp_queue_clear_locked(&pcb->tx_buf.unacked);
        tcp_timers_reset(pcb);
        socket = pcb->socket;
        mutex_unlock(&pcb->lock);
        if (socket != 0) {
            tcp_socket_notify(socket);
        } else if (half_open_child) {
            /* Half-open child from passive LISTEN without a userspace socket. */
            tcp_socket_destroy(pcb);
        }
        return 0;
    }

    if (pcb->state == TCP_SYN_SENT &&
        (flags & (TCP_FLAG_SYN | TCP_FLAG_ACK)) ==
            (TCP_FLAG_SYN | TCP_FLAG_ACK) &&
        (acknowledgement == pcb->iss + 1U || acknowledgement == pcb->seq_number)) {
        pcb->ack_number = sequence + 1U;
        pcb->rcv_nxt = sequence + 1U;
        pcb->snd_una = acknowledgement;
        pcb->snd_nxt = pcb->iss + 1U;
        pcb->seq_number = pcb->iss + 1U;
        pcb->state = TCP_ESTABLISHED;
        tcp_timers_reset(pcb);
        pcb->timers.keepalive = kernel_ticks + TCP_KEEPALIVE_TICKS;
        pcb->flags |= TCP_PCB_FLAG_TIMER_KEEP | TCP_PCB_FLAG_ACTIVE;
        send_out = 1;
        (void)tcp_build_output_locked(pcb, TCP_FLAG_ACK, 0, 0, out_packet,
            &out_packet_len, &out_remote_ip);
        klog("[TCP RX] SYN+ACK recebido -> conexao ESTABLISHED, ACK transmitido\n");
    } else if (pcb->state == TCP_SYN_RECEIVED) {
        if ((flags & TCP_FLAG_SYN) != 0U && (flags & TCP_FLAG_ACK) == 0U) {
            /* Client retransmitted SYN: retransmit SYN+ACK */
            send_out = 1;
            (void)tcp_build_output_locked(pcb,
                (uint8_t)(TCP_FLAG_SYN | TCP_FLAG_ACK), 0, 0,
                out_packet, &out_packet_len, &out_remote_ip);
            klog("[TCP TX] SYN+ACK retransmitido para SYN duplicado\n");
        } else if ((flags & TCP_FLAG_ACK) != 0U && (flags & TCP_FLAG_SYN) == 0U &&
            (acknowledgement == pcb->iss + 1U || acknowledgement == pcb->seq_number)) {
            struct tcp_pcb *listener = pcb->parent;
            pcb->state = TCP_ESTABLISHED;

            tcp_timers_reset(pcb);
            pcb->timers.keepalive = kernel_ticks + TCP_KEEPALIVE_TICKS;
            pcb->flags |= TCP_PCB_FLAG_TIMER_KEEP | TCP_PCB_FLAG_ACTIVE;
            pcb->snd_una = acknowledgement;
            pcb->snd_nxt = acknowledgement;
            pcb->seq_number = acknowledgement;

            if (listener != 0) {
                mutex_unlock(&pcb->lock);
                mutex_lock(&listener->lock);
                mutex_lock(&pcb->lock);
                if (listener->state == TCP_LISTEN &&
                    tcp_accept_enqueue_locked(listener, pcb) == 0) {
                    listener_socket = listener->socket;
                    klog("[TCP RX] ACK recebido -> conexao ESTABLISHED inserida no backlog\n");
                } else {
                    /* Backlog full or listener closed: drop the child */
                    mutex_unlock(&pcb->lock);
                    mutex_unlock(&listener->lock);
                    tcp_socket_destroy(pcb);
                    return -1;
                }
                mutex_unlock(&pcb->lock);
                mutex_unlock(&listener->lock);
                if (listener_socket != 0) {
                    tcp_socket_notify(listener_socket);
                }
                return 0;
            }
        }
    } else if (pcb->state == TCP_ESTABLISHED) {
        /* If ACK flag is present, process data acknowledgements */
        if ((flags & TCP_FLAG_ACK) != 0U) {
            tcp_tx_ack_received_locked(pcb, acknowledgement);
        }

        /* 1. Incoming data payload processing */
        if (payload_len != 0U) {
            if (TCP_SEQ_EQ(sequence, pcb->rcv_nxt)) {
                /* In-order data */
                size_t space = tcp_rx_buffer_free_space_locked(pcb);
                size_t to_write = (payload_len < space) ? payload_len : space;
                if (to_write > 0) {
                    size_t written = tcp_rx_buffer_write_locked(pcb, payload, to_write);
                    pcb->rcv_nxt += (uint32_t)written;
                    pcb->ack_number = pcb->rcv_nxt;
                }
                size_t free_sp = tcp_rx_buffer_free_space_locked(pcb);
                pcb->window = (uint32_t)free_sp;
                pcb->rcv_wnd = (uint16_t)(free_sp & 0xFFFFU);

                send_out = 1;
                (void)tcp_build_output_locked(pcb, TCP_FLAG_ACK, 0, 0, out_packet,
                    &out_packet_len, &out_remote_ip);
                klog("[TCP RX] In-order data armazenado no rx_buf, ACK transmitido\n");
            } else if (TCP_SEQ_LT(sequence, pcb->rcv_nxt)) {
                /* Duplicate segment: drop payload, re-send ACK with current rcv_nxt */
                size_t free_sp = tcp_rx_buffer_free_space_locked(pcb);
                pcb->window = (uint32_t)free_sp;
                pcb->rcv_wnd = (uint16_t)(free_sp & 0xFFFFU);
                pcb->ack_number = pcb->rcv_nxt;
                send_out = 1;
                (void)tcp_build_output_locked(pcb, TCP_FLAG_ACK, 0, 0, out_packet,
                    &out_packet_len, &out_remote_ip);
                klog("[TCP RX] Data duplicado descartado, ACK retransmitido\n");
            } else {
                /* Out-of-order segment: drop payload per Phase 7 policy, re-send ACK with current rcv_nxt */
                size_t free_sp = tcp_rx_buffer_free_space_locked(pcb);
                pcb->window = (uint32_t)free_sp;
                pcb->rcv_wnd = (uint16_t)(free_sp & 0xFFFFU);
                pcb->ack_number = pcb->rcv_nxt;
                send_out = 1;
                (void)tcp_build_output_locked(pcb, TCP_FLAG_ACK, 0, 0, out_packet,
                    &out_packet_len, &out_remote_ip);
                klog("[TCP RX] Data fora de ordem descartado, ACK retransmitido\n");
            }
        }

        /* 2. FIN handling */
        if ((flags & TCP_FLAG_FIN) != 0U) {
            if (TCP_SEQ_EQ(sequence + (uint32_t)payload_len, pcb->rcv_nxt)) {
                pcb->rcv_nxt += 1U;
                pcb->ack_number = pcb->rcv_nxt;
                pcb->flags |= TCP_PCB_FLAG_EOF;
                pcb->state = TCP_CLOSE_WAIT;

                size_t free_sp = tcp_rx_buffer_free_space_locked(pcb);
                pcb->window = (uint32_t)free_sp;
                pcb->rcv_wnd = (uint16_t)(free_sp & 0xFFFFU);
                send_out = 1;
                (void)tcp_build_output_locked(pcb, TCP_FLAG_ACK, 0, 0, out_packet,
                    &out_packet_len, &out_remote_ip);
                klog("[TCP RX] FIN recebido -> CLOSE_WAIT, ACK transmitido\n");
            }
        }
    }

    socket = pcb->socket;
    mutex_unlock(&pcb->lock);

    if (socket != 0) {
        tcp_socket_notify(socket);
    }
    if (send_out) {
        (void)net_send_ipv4(out_remote_ip, IP_PROTO_TCP, out_packet,
            out_packet_len);
    }
    return 0;
}

size_t tcp_rx_buffer_available_locked(struct tcp_pcb *pcb)
{
    if (pcb == 0) {
        return 0;
    }
    return pcb->rx_buf.used;
}

size_t tcp_rx_buffer_free_space_locked(struct tcp_pcb *pcb)
{
    if (pcb == 0 || pcb->rx_buf.data == 0) {
        return 0;
    }
    if (pcb->rx_buf.used >= pcb->rx_buf.capacity) {
        return 0;
    }
    return pcb->rx_buf.capacity - pcb->rx_buf.used;
}

size_t tcp_rx_buffer_write_locked(struct tcp_pcb *pcb, const uint8_t *src, size_t len)
{
    if (pcb == 0 || src == 0 || len == 0 || pcb->rx_buf.data == 0) {
        return 0;
    }
    size_t space = tcp_rx_buffer_free_space_locked(pcb);
    size_t to_write = (len < space) ? len : space;
    if (to_write == 0) {
        return 0;
    }

    size_t cap = pcb->rx_buf.capacity;
    size_t tail = pcb->rx_buf.tail;
    size_t first_chunk = cap - tail;
    if (to_write <= first_chunk) {
        tcp_copy(pcb->rx_buf.data + tail, src, to_write);
        pcb->rx_buf.tail = (tail + to_write) % cap;
    } else {
        tcp_copy(pcb->rx_buf.data + tail, src, first_chunk);
        tcp_copy(pcb->rx_buf.data, src + first_chunk, to_write - first_chunk);
        pcb->rx_buf.tail = to_write - first_chunk;
    }
    pcb->rx_buf.used += to_write;
    return to_write;
}

size_t tcp_rx_buffer_read_locked(struct tcp_pcb *pcb, uint8_t *dst, size_t len)
{
    if (pcb == 0 || dst == 0 || len == 0 || pcb->rx_buf.data == 0) {
        return 0;
    }
    size_t available = pcb->rx_buf.used;
    size_t to_read = (len < available) ? len : available;
    if (to_read == 0) {
        return 0;
    }

    size_t cap = pcb->rx_buf.capacity;
    size_t head = pcb->rx_buf.head;
    size_t first_chunk = cap - head;
    if (to_read <= first_chunk) {
        tcp_copy(dst, pcb->rx_buf.data + head, to_read);
        pcb->rx_buf.head = (head + to_read) % cap;
    } else {
        tcp_copy(dst, pcb->rx_buf.data + head, first_chunk);
        tcp_copy(dst + first_chunk, pcb->rx_buf.data, to_read - first_chunk);
        pcb->rx_buf.head = to_read - first_chunk;
    }
    pcb->rx_buf.used -= to_read;
    return to_read;
}

int tcp_receive_read(struct tcp_pcb *pcb, uint8_t *buffer, size_t length)
{
    if (pcb == 0 || buffer == 0 || length == 0U) {
        return -1;
    }
    mutex_lock(&pcb->lock);
    size_t read_bytes = tcp_rx_buffer_read_locked(pcb, buffer, length);
    size_t free_sp = tcp_rx_buffer_free_space_locked(pcb);
    pcb->window = (uint32_t)free_sp;
    pcb->rcv_wnd = (uint16_t)(free_sp & 0xFFFFU);
    mutex_unlock(&pcb->lock);
    return (int)read_bytes;
}

size_t tcp_receive_available(struct tcp_pcb *pcb)
{
    if (pcb == 0) {
        return 0;
    }
    mutex_lock(&pcb->lock);
    size_t bytes = tcp_rx_buffer_available_locked(pcb);
    mutex_unlock(&pcb->lock);
    return bytes;
}

size_t tcp_tx_buffer_available_locked(struct tcp_pcb *pcb)
{
    if (pcb == 0) {
        return 0;
    }
    size_t used = tcp_tx_buffer_used_locked(pcb);
    if (used >= pcb->tx_buf.capacity) {
        return 0;
    }
    return pcb->tx_buf.capacity - used;
}

void tcp_tx_ack_received_locked(struct tcp_pcb *pcb, uint32_t ack)
{
    if (pcb == 0) {
        return;
    }

    /* 1. Invalid ACK: acknowledges sequence space beyond what we sent */
    if (TCP_SEQ_GT(ack, pcb->snd_nxt)) {
        klog("TCP: ACK invalido ignorado (ACK > SND.NXT)\n");
        return;
    }

    /* 2. Duplicate ACK: ACK == SND.UNA (no new data confirmed) */
    if (TCP_SEQ_EQ(ack, pcb->snd_una)) {
        return;
    }

    /* 3. Stale ACK: ACK < SND.UNA */
    if (TCP_SEQ_LT(ack, pcb->snd_una)) {
        return;
    }

    /* 4. Valid new ACK: SND.UNA < ACK <= SND.NXT */
    pcb->snd_una = ack;

    /* Free confirmed segments or adjust partially confirmed segment */
    while (pcb->tx_buf.unacked.head != 0) {
        struct tcp_segment *seg = pcb->tx_buf.unacked.head;
        uint32_t seg_end = seg->sequence + (uint32_t)seg->length;

        if (TCP_SEQ_LE(seg_end, ack)) {
            /* Entire segment is acknowledged */
            (void)tcp_queue_take_head_locked(&pcb->tx_buf.unacked);
            if (seg->data != 0) {
                kfree(seg->data);
                seg->data = 0;
            }
            kfree(seg);
        } else if (TCP_SEQ_LT(seg->sequence, ack)) {
            /* Partial ACK within this segment */
            size_t bytes_acked = (size_t)(uint32_t)(ack - seg->sequence);
            if (bytes_acked >= seg->length) {
                /* Defensive guard: complete ACKs are handled by the branch above. */
                break;
            }
            seg->sequence = ack;
            seg->length -= bytes_acked;
            seg->offset += bytes_acked;
            if (pcb->tx_buf.unacked.bytes >= bytes_acked) {
                pcb->tx_buf.unacked.bytes -= bytes_acked;
            } else {
                pcb->tx_buf.unacked.bytes = 0;
            }
            break;
        } else {
            /* seg->sequence >= ack: unacknowledged segment */
            break;
        }
    }

    /* Timer management */
    if (pcb->tx_buf.unacked.head == 0) {
        /* All data acknowledged: clear retransmission timer */
        pcb->flags &= ~TCP_PCB_FLAG_TIMER_RTO;
        pcb->timers.retransmission = 0;
        pcb->retransmission_timer = 0;
        pcb->timers.retransmit_count = 0;
        pcb->timers.rto_ticks = (uint32_t)TCP_DATA_RTO_TICKS_DEFAULT;
    } else {
        /* Reset retransmission timer for remaining unacknowledged data */
        pcb->timers.rto_ticks = (uint32_t)TCP_DATA_RTO_TICKS_DEFAULT;
        pcb->timers.retransmit_count = 0;
        pcb->timers.retransmission = kernel_ticks + pcb->timers.rto_ticks;
        pcb->retransmission_timer = pcb->timers.retransmission;
        pcb->flags |= TCP_PCB_FLAG_TIMER_RTO;
    }
}

int tcp_send(struct tcp_pcb *pcb, const uint8_t *buffer, size_t len)
{
    if (pcb == 0 || buffer == 0) {
        return -1;
    }
    if (len == 0) {
        return 0;
    }

    size_t accepted = 0;
    while (accepted < len) {
        size_t requested = len - accepted;
        if (requested > TCP_DEFAULT_MSS) {
            requested = TCP_DEFAULT_MSS;
        }

        /* Allocate the private kernel copy before taking the PCB mutex. */
        struct tcp_segment *seg = (struct tcp_segment *)kmalloc(sizeof(*seg));
        if (seg == 0) {
            return accepted != 0 ? (int)accepted : -1;
        }
        seg->data = (uint8_t *)kmalloc(requested);
        if (seg->data == 0) {
            kfree(seg);
            return accepted != 0 ? (int)accepted : -1;
        }
        tcp_copy(seg->data, buffer + accepted, requested);

        mutex_lock(&pcb->lock);
        if (pcb->state != TCP_ESTABLISHED ||
            (pcb->flags & TCP_PCB_FLAG_RESET) != 0) {
            mutex_unlock(&pcb->lock);
            kfree(seg->data);
            kfree(seg);
            return accepted != 0 ? (int)accepted : -1;
        }

        size_t space = tcp_tx_buffer_available_locked(pcb);
        if (space == 0) {
            mutex_unlock(&pcb->lock);
            kfree(seg->data);
            kfree(seg);
            return accepted != 0 ? (int)accepted : -1;
        }
        size_t chunk_len = requested < space ? requested : space;
        seg->sequence = pcb->snd_nxt;
        seg->length = chunk_len;
        seg->offset = 0;
        seg->sent_at = 0;
        seg->retransmit_count = 0;
        tcp_queue_append_locked(&pcb->tx_buf.unsent, seg);

        /*
         * Send immediately in this phase.  Moving the entry before dropping
         * pcb->lock makes a synchronous peer ACK safe: tcp_input can only
         * acknowledge data already represented in the unacked queue.
         */
        seg = tcp_queue_take_head_locked(&pcb->tx_buf.unsent);
        uint32_t cur_seq = seg->sequence;
        int queue_was_empty = pcb->tx_buf.unacked.head == 0;
        tcp_queue_append_locked(&pcb->tx_buf.unacked, seg);
        pcb->snd_nxt += (uint32_t)chunk_len;
        pcb->seq_number = pcb->snd_nxt;
        seg->sent_at = kernel_ticks;

        if (queue_was_empty) {
            pcb->timers.rto_ticks = (uint32_t)TCP_DATA_RTO_TICKS_DEFAULT;
            pcb->timers.retransmission = kernel_ticks + pcb->timers.rto_ticks;
            pcb->retransmission_timer = pcb->timers.retransmission;
            pcb->timers.retransmit_count = 0;
            pcb->flags |= TCP_PCB_FLAG_TIMER_RTO;
        }

        /* Build the wire packet under the lock, then release it before IPv4. */
        uint8_t packet[TCP_HEADER_MIN_SIZE + TCP_DEFAULT_MSS];
        size_t packet_len = sizeof(struct tcp_header) + chunk_len;
        uint32_t local_ip = tcp_effective_local_ip(pcb);
        uint32_t remote_ip = pcb->remote_ip;

        struct tcp_header *hdr = (struct tcp_header *)packet;
        hdr->src_port = htons(pcb->local_port);
        hdr->dest_port = htons(pcb->remote_port);
        hdr->seq_num = htonl(cur_seq);
        hdr->ack_num = htonl(pcb->rcv_nxt);
        hdr->data_offset_flags = htons((5U << 12) | TCP_FLAG_ACK | TCP_FLAG_PSH);
        hdr->window_size = htons((uint16_t)(pcb->window & 0xFFFFU));
        hdr->checksum = 0;
        hdr->urgent_ptr = 0;

        tcp_copy(packet + sizeof(struct tcp_header), seg->data, chunk_len);
        uint16_t csum = tcp_checksum(local_ip, remote_ip, packet, packet_len);
        if (csum == 0U) {
            csum = 0xFFFFU;
        }
        hdr->checksum = htons(csum);

        mutex_unlock(&pcb->lock);

        /* Transmit with NO locks held */
        klog("[TCP TX] DATA transmitido\n");
        if (net_send_ipv4(remote_ip, IP_PROTO_TCP, packet, packet_len) != 0) {
            /* The private unacked copy remains queued for the data RTO. */
            klog("[TCP TX] DATA enfileirado para retransmissao apos falha de IPv4\n");
        }

        accepted += chunk_len;
    }

    return (int)accepted;
}

void tcp_run_tests(void)
{
    klog("\n========================================\n");
    klog("[TCP TEST] Iniciando Suite de Testes TCP...\n");

    /* Test 1: Alloc */
    tcp_pcb_t *pcb = tcp_alloc();
    if (pcb != 0 && pcb->state == TCP_CLOSED && pcb->rcv_wnd == TCP_DEFAULT_WINDOW) {
        klog("[TCP TEST] PASS: tcp_alloc (PCB criado com sucesso e estado CLOSED)\n");
    } else {
        klog("[TCP TEST] FAIL: tcp_alloc\n");
    }

    /* Test 2: Register */
    pcb->local_ip = 0x0A00020F; /* 10.0.2.15 */
    pcb->local_port = 12345;
    pcb->remote_ip = 0x0A000202; /* 10.0.2.2 */
    pcb->remote_port = 80;
    if (tcp_register(pcb) == 0) {
        klog("[TCP TEST] PASS: tcp_register (PCB registrado na lista global)\n");
    } else {
        klog("[TCP TEST] FAIL: tcp_register\n");
    }

    /* Test 3: Lookup */
    tcp_pcb_t *found = tcp_lookup(0x0A00020F, 0x0A000202, 12345, 80);
    if (found == pcb) {
        klog("[TCP TEST] PASS: tcp_lookup (PCB localizado por 4-tuple)\n");
    } else {
        klog("[TCP TEST] FAIL: tcp_lookup\n");
    }

    /* Test 4: Header Serializer & Parser */
    struct tcp_header orig_hdr;
    struct tcp_header parsed_hdr;
    uint8_t hdr_buf[TCP_HEADER_MIN_SIZE];
    orig_hdr.src_port = 1234;
    orig_hdr.dest_port = 80;
    orig_hdr.seq_num = 0x12345678;
    orig_hdr.ack_num = 0x87654321;
    orig_hdr.data_offset_flags = (5U << 12) | TCP_FLAG_SYN;
    orig_hdr.window_size = 8192;
    orig_hdr.checksum = 0;
    orig_hdr.urgent_ptr = 0;

    if (tcp_serialize_header(&orig_hdr, hdr_buf, sizeof(hdr_buf)) == sizeof(hdr_buf) &&
        tcp_parse_header(hdr_buf, sizeof(hdr_buf), &parsed_hdr) == 0 &&
        parsed_hdr.src_port == 1234 && parsed_hdr.dest_port == 80 &&
        parsed_hdr.seq_num == 0x12345678 && parsed_hdr.ack_num == 0x87654321 &&
        parsed_hdr.window_size == 8192) {
        klog("[TCP TEST] PASS: tcp_serialize_header e tcp_parse_header\n");
    } else {
        klog("[TCP TEST] FAIL: tcp_serialize_header / tcp_parse_header\n");
    }

    /* Test 5: Checksum */
    uint32_t src_ip = 0x0A00020F;
    uint32_t dest_ip = 0x0A000202;
    uint16_t csum = tcp_checksum(src_ip, dest_ip, hdr_buf, sizeof(hdr_buf));
    ((struct tcp_header *)hdr_buf)->checksum = htons(csum);
    if (tcp_checksum(src_ip, dest_ip, hdr_buf, sizeof(hdr_buf)) == 0U) {
        klog("[TCP TEST] PASS: tcp_checksum (Checksum calculado e validado)\n");
    } else {
        klog("[TCP TEST] FAIL: tcp_checksum\n");
    }

    /* Test 6: Unregister & Free */
    tcp_unregister(pcb);
    found = tcp_lookup(0x0A00020F, 0x0A000202, 12345, 80);
    if (found == 0) {
        tcp_free(pcb);
        klog("[TCP TEST] PASS: tcp_unregister e tcp_free (PCB removido e liberado)\n");
    } else {
        klog("[TCP TEST] FAIL: tcp_unregister / tcp_free\n");
    }

    /* Test 7: Socket Integration & Non-Regression */
    int fd_tcp = sys_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    int fd_raw = sys_socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    int fd_udp = sys_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

    if (fd_tcp >= 0 && fd_raw >= 0 && fd_udp >= 0) {
        struct sockaddr_in bind_addr;
        bind_addr.sin_family = AF_INET;
        bind_addr.sin_port = htons(8080);
        bind_addr.sin_addr.s_addr = htonl(0x0A00020F);

        int bind_res = sys_bind(fd_tcp, (const struct sockaddr *)&bind_addr, sizeof(bind_addr));
        if (bind_res == 0) {
            klog("[TCP TEST] PASS: sys_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP) e sys_bind()\n");
        } else {
            klog("[TCP TEST] FAIL: sys_bind TCP\n");
        }

        sys_close(fd_tcp);
        sys_close(fd_raw);
        sys_close(fd_udp);
        klog("[TCP TEST] PASS: sys_socket integration e nao-regressao RAW/DGRAM\n");
    } else {
        if (fd_tcp >= 0) sys_close(fd_tcp);
        if (fd_raw >= 0) sys_close(fd_raw);
        if (fd_udp >= 0) sys_close(fd_udp);
        klog("[TCP TEST] FAIL: sys_socket integration\n");
    }

    /* Test 8: State Machine & Active Handshake Simulation (Phase 2A) */
    tcp_pcb_t *hs_pcb = tcp_alloc();
    if (hs_pcb != 0) {
        hs_pcb->local_ip = htonl(0x0A00020FU);
        hs_pcb->remote_ip = htonl(0x0A000202U);
        hs_pcb->local_port = 49160;
        hs_pcb->remote_port = 8080;
        hs_pcb->iss = 1000;
        hs_pcb->seq_number = hs_pcb->iss;
        hs_pcb->ack_number = 0;
        hs_pcb->state = TCP_SYN_SENT;
        tcp_register(hs_pcb);

        /* Simulates incoming SYN+ACK from peer */
        struct tcp_header synack_hdr;
        uint8_t synack_buf[TCP_HEADER_MIN_SIZE];
        synack_hdr.src_port = 8080;
        synack_hdr.dest_port = 49160;
        synack_hdr.seq_num = 5000; /* Peer ISN */
        synack_hdr.ack_num = 1001; /* Peer ACK of our ISS+1 */
        synack_hdr.data_offset_flags = (5U << 12) | TCP_FLAG_SYN | TCP_FLAG_ACK;
        synack_hdr.window_size = 65535;
        synack_hdr.checksum = 0;
        synack_hdr.urgent_ptr = 0;

        tcp_serialize_header(&synack_hdr, synack_buf, sizeof(synack_buf));
        uint16_t hs_csum = tcp_checksum(htonl(0x0A000202U), htonl(0x0A00020FU), synack_buf, sizeof(synack_buf));
        ((struct tcp_header *)synack_buf)->checksum = htons(hs_csum);

        int input_res = tcp_input(htonl(0x0A000202U), htonl(0x0A00020FU), synack_buf, sizeof(synack_buf));
        if (input_res == 0 && hs_pcb->state == TCP_ESTABLISHED && hs_pcb->ack_number == 5001) {
            klog("[TCP TEST] PASS: 3-Way Handshake (SYN_SENT -> SYN+ACK -> ESTABLISHED)\n");
        } else {
            klog("[TCP TEST] FAIL: 3-Way Handshake Simulation\n");
        }

        /* Test 9: RST reception handling */
        struct tcp_header rst_hdr;
        uint8_t rst_buf[TCP_HEADER_MIN_SIZE];
        rst_hdr.src_port = 8080;
        rst_hdr.dest_port = 49160;
        rst_hdr.seq_num = 5001;
        rst_hdr.ack_num = 1001;
        rst_hdr.data_offset_flags = (5U << 12) | TCP_FLAG_RST;
        rst_hdr.window_size = 0;
        rst_hdr.checksum = 0;
        rst_hdr.urgent_ptr = 0;

        tcp_serialize_header(&rst_hdr, rst_buf, sizeof(rst_buf));
        uint16_t rst_csum = tcp_checksum(htonl(0x0A000202U), htonl(0x0A00020FU), rst_buf, sizeof(rst_buf));
        ((struct tcp_header *)rst_buf)->checksum = htons(rst_csum);

        int rst_res = tcp_input(htonl(0x0A000202U), htonl(0x0A00020FU), rst_buf, sizeof(rst_buf));
        if (rst_res == 0 && hs_pcb->state == TCP_CLOSED) {
            klog("[TCP TEST] PASS: RST handling (transicao para CLOSED)\n");
        } else {
            klog("[TCP TEST] FAIL: RST handling\n");
        }

        tcp_free(hs_pcb);
    }

    /* Test 10: Timer ticks & Timeout mechanics */
    tcp_pcb_t *tm_pcb = tcp_alloc();
    if (tm_pcb != 0) {
        tm_pcb->local_ip = htonl(0x0A00020FU);
        tm_pcb->remote_ip = htonl(0x0A000202U);
        tm_pcb->local_port = 49161;
        tm_pcb->remote_port = 9999;
        tm_pcb->iss = 2000;
        tm_pcb->state = TCP_SYN_SENT;
        tm_pcb->timers.timeout = 500ULL;
        tm_pcb->timers.retransmission = 1000ULL;
        tm_pcb->timers.rto_ticks = 100;
        tm_pcb->timers.retransmit_count = 0;
        tcp_register(tm_pcb);

        tcp_timer_tick(600ULL);
        if (tm_pcb->state == TCP_CLOSED) {
            klog("[TCP TEST] PASS: tcp_timer_tick (timeout expira e fecha PCB)\n");
        } else {
            klog("[TCP TEST] FAIL: tcp_timer_tick timeout\n");
        }
        tcp_free(tm_pcb);
    }

    /* Test 11: Passive Open - tcp_listen and backlog verification */
    tcp_pcb_t *lst_pcb = tcp_alloc();
    if (lst_pcb != 0) {
        lst_pcb->local_ip = htonl(0x0A00020FU);
        lst_pcb->local_port = 8888;
        tcp_register(lst_pcb);

        int lis_res = tcp_listen(lst_pcb, 100); /* Exceeds MAX_BACKLOG, should clamp */
        if (lis_res == 0 && lst_pcb->state == TCP_LISTEN &&
            lst_pcb->backlog == (int)TCP_MAX_BACKLOG &&
            (lst_pcb->flags & TCP_PCB_FLAG_PASSIVE) != 0) {
            klog("[TCP TEST] PASS: tcp_listen (estado LISTEN, flag PASSIVE, backlog clamp)\n");
        } else {
            klog("[TCP TEST] FAIL: tcp_listen\n");
        }
        tcp_socket_destroy(lst_pcb);
    }

    /* Test 12: Passive Open - Handshake, child PCB, backlog enqueue & accept */
    tcp_pcb_t *srv_pcb = tcp_alloc();
    if (srv_pcb != 0) {
        srv_pcb->local_ip = htonl(0x0A00020FU);
        srv_pcb->local_port = 8889;
        tcp_register(srv_pcb);
        tcp_listen(srv_pcb, 4);

        /* Simulates incoming SYN from remote client */
        struct tcp_header syn_hdr;
        uint8_t syn_buf[TCP_HEADER_MIN_SIZE];
        syn_hdr.src_port = 54321;
        syn_hdr.dest_port = 8889;
        syn_hdr.seq_num = 10000; /* Client ISS */
        syn_hdr.ack_num = 0;
        syn_hdr.data_offset_flags = (5U << 12) | TCP_FLAG_SYN;
        syn_hdr.window_size = 65535;
        syn_hdr.checksum = 0;
        syn_hdr.urgent_ptr = 0;

        tcp_serialize_header(&syn_hdr, syn_buf, sizeof(syn_buf));
        uint16_t syn_csum = tcp_checksum(htonl(0x0A000202U), htonl(0x0A00020FU), syn_buf, sizeof(syn_buf));
        ((struct tcp_header *)syn_buf)->checksum = htons(syn_csum);

        int syn_res = tcp_input(htonl(0x0A000202U), htonl(0x0A00020FU), syn_buf, sizeof(syn_buf));
        tcp_pcb_t *child = tcp_lookup(htonl(0x0A00020FU), htonl(0x0A000202U), 8889, 54321);

        if (syn_res == 0 && child != 0 && child->state == TCP_SYN_RECEIVED &&
            child->ack_number == 10001 && child->parent == srv_pcb &&
            srv_pcb->state == TCP_LISTEN) {
            klog("[TCP TEST] PASS: Passive Handshake SYN (Child criado em SYN_RECEIVED, listener permanece LISTEN)\n");

            /* Simulates incoming final ACK from client */
            struct tcp_header ack_hdr;
            uint8_t ack_buf[TCP_HEADER_MIN_SIZE];
            ack_hdr.src_port = 54321;
            ack_hdr.dest_port = 8889;
            ack_hdr.seq_num = 10001;
            ack_hdr.ack_num = child->iss + 1U;
            ack_hdr.data_offset_flags = (5U << 12) | TCP_FLAG_ACK;
            ack_hdr.window_size = 65535;
            ack_hdr.checksum = 0;
            ack_hdr.urgent_ptr = 0;

            tcp_serialize_header(&ack_hdr, ack_buf, sizeof(ack_buf));
            uint16_t ack_csum = tcp_checksum(htonl(0x0A000202U), htonl(0x0A00020FU), ack_buf, sizeof(ack_buf));
            ((struct tcp_header *)ack_buf)->checksum = htons(ack_csum);

            int ack_res = tcp_input(htonl(0x0A000202U), htonl(0x0A00020FU), ack_buf, sizeof(ack_buf));
            if (ack_res == 0 && child->state == TCP_ESTABLISHED && srv_pcb->accept_count == 1) {
                klog("[TCP TEST] PASS: Final ACK (Child ESTABLISHED e inserido no backlog do listener)\n");

                tcp_pcb_t *accepted = tcp_accept(srv_pcb);
                if (accepted == child && accepted->parent == 0 && srv_pcb->accept_count == 0) {
                    klog("[TCP TEST] PASS: tcp_accept (Child desempilhado com sucesso, parent desvinculado)\n");
                } else {
                    klog("[TCP TEST] FAIL: tcp_accept dequeue\n");
                }
            } else {
                klog("[TCP TEST] FAIL: Final ACK processing\n");
            }
            tcp_socket_destroy(child);
        } else {
            klog("[TCP TEST] FAIL: Passive Handshake SYN\n");
            if (child != 0) tcp_socket_destroy(child);
        }
        tcp_socket_destroy(srv_pcb);
    }

    /* Test 13: Ring Buffer operations (tcp_rx_buffer) */
    tcp_pcb_t *rb_pcb = tcp_alloc();
    if (rb_pcb != 0) {
        const char *t_data = "ABCDEFGH";
        size_t wr = tcp_rx_buffer_write_locked(rb_pcb, (const uint8_t *)t_data, 8);
        uint8_t r_buf[16];
        size_t rd1 = tcp_rx_buffer_read_locked(rb_pcb, r_buf, 4);
        size_t rd2 = tcp_rx_buffer_read_locked(rb_pcb, r_buf + 4, 4);
        r_buf[8] = '\0';

        if (wr == 8 && rd1 == 4 && rd2 == 4 && rb_pcb->rx_buf.used == 0 &&
            r_buf[0] == 'A' && r_buf[3] == 'D' && r_buf[4] == 'E' && r_buf[7] == 'H') {
            klog("[TCP TEST] PASS: tcp_rx_buffer (escrita circular, leitura parcial e used tracking)\n");
        } else {
            klog("[TCP TEST] FAIL: tcp_rx_buffer\n");
        }
        tcp_free(rb_pcb);
    }

    /* Test 14: Receive Path Simulation (in-order data, duplicate, out-of-order, FIN) */
    tcp_pcb_t *rx_pcb = tcp_alloc();
    if (rx_pcb != 0) {
        rx_pcb->local_ip = htonl(0x0A00020FU);
        rx_pcb->remote_ip = htonl(0x0A000202U);
        rx_pcb->local_port = 7777;
        rx_pcb->remote_port = 55555;
        rx_pcb->iss = 50000;
        rx_pcb->snd_una = 50001;
        rx_pcb->snd_nxt = 50001;
        rx_pcb->seq_number = 50001;
        rx_pcb->rcv_nxt = 10000;
        rx_pcb->ack_number = 10000;
        rx_pcb->state = TCP_ESTABLISHED;
        tcp_register(rx_pcb);

        /* Simulates in-order incoming DATA: seq=10000, len=5 ("HELLO") */
        uint8_t data_pkt[TCP_HEADER_MIN_SIZE + 5];
        struct tcp_header d_hdr;
        d_hdr.src_port = 55555;
        d_hdr.dest_port = 7777;
        d_hdr.seq_num = 10000;
        d_hdr.ack_num = 50001;
        d_hdr.data_offset_flags = (5U << 12) | TCP_FLAG_ACK;
        d_hdr.window_size = 65535;
        d_hdr.checksum = 0;
        d_hdr.urgent_ptr = 0;
        tcp_serialize_header(&d_hdr, data_pkt, sizeof(data_pkt));
        tcp_copy(data_pkt + TCP_HEADER_MIN_SIZE, "HELLO", 5);
        uint16_t d_csum = tcp_checksum(htonl(0x0A000202U), htonl(0x0A00020FU), data_pkt, sizeof(data_pkt));
        ((struct tcp_header *)data_pkt)->checksum = htons(d_csum);

        int d_res = tcp_input(htonl(0x0A000202U), htonl(0x0A00020FU), data_pkt, sizeof(data_pkt));
        uint8_t read_out[10];
        int rd_bytes = tcp_receive_read(rx_pcb, read_out, sizeof(read_out));

        if (d_res == 0 && rx_pcb->rcv_nxt == 10005 && rd_bytes == 5 &&
            read_out[0] == 'H' && read_out[4] == 'O') {
            klog("[TCP TEST] PASS: Receive Path (DATA in-order recebido, rcv_nxt avancado e lido do buffer)\n");
        } else {
            klog("[TCP TEST] FAIL: Receive Path DATA in-order\n");
        }

        /* Simulates duplicate DATA (seq=10000 < rcv_nxt=10005): should be dropped */
        int dup_res = tcp_input(htonl(0x0A000202U), htonl(0x0A00020FU), data_pkt, sizeof(data_pkt));
        if (dup_res == 0 && rx_pcb->rcv_nxt == 10005 && rx_pcb->rx_buf.used == 0) {
            klog("[TCP TEST] PASS: Receive Path (DATA duplicado descartado sem poluir rx_buf)\n");
        } else {
            klog("[TCP TEST] FAIL: Receive Path DATA duplicate\n");
        }

        /* Simulates out-of-order DATA (seq=10020 > rcv_nxt=10005): should be dropped */
        d_hdr.seq_num = 10020;
        tcp_serialize_header(&d_hdr, data_pkt, sizeof(data_pkt));
        tcp_copy(data_pkt + TCP_HEADER_MIN_SIZE, "WORLD", 5);
        d_csum = tcp_checksum(htonl(0x0A000202U), htonl(0x0A00020FU), data_pkt, sizeof(data_pkt));
        ((struct tcp_header *)data_pkt)->checksum = htons(d_csum);

        int ooo_res = tcp_input(htonl(0x0A000202U), htonl(0x0A00020FU), data_pkt, sizeof(data_pkt));
        if (ooo_res == 0 && rx_pcb->rcv_nxt == 10005 && rx_pcb->rx_buf.used == 0) {
            klog("[TCP TEST] PASS: Receive Path (DATA out-of-order descartado por politica sem avancar rcv_nxt)\n");
        } else {
            klog("[TCP TEST] FAIL: Receive Path DATA out-of-order\n");
        }

        /* Simulates FIN (seq=10005 == rcv_nxt=10005): should advance rcv_nxt to 10006 and set CLOSE_WAIT */
        uint8_t fin_pkt[TCP_HEADER_MIN_SIZE];
        struct tcp_header fin_hdr;
        fin_hdr.src_port = 55555;
        fin_hdr.dest_port = 7777;
        fin_hdr.seq_num = 10005;
        fin_hdr.ack_num = 50001;
        fin_hdr.data_offset_flags = (5U << 12) | TCP_FLAG_ACK | TCP_FLAG_FIN;
        fin_hdr.window_size = 65535;
        fin_hdr.checksum = 0;
        fin_hdr.urgent_ptr = 0;
        tcp_serialize_header(&fin_hdr, fin_pkt, sizeof(fin_pkt));
        uint16_t fin_csum = tcp_checksum(htonl(0x0A000202U), htonl(0x0A00020FU), fin_pkt, sizeof(fin_pkt));
        ((struct tcp_header *)fin_pkt)->checksum = htons(fin_csum);

        int fin_res = tcp_input(htonl(0x0A000202U), htonl(0x0A00020FU), fin_pkt, sizeof(fin_pkt));
        if (fin_res == 0 && rx_pcb->rcv_nxt == 10006 && rx_pcb->state == TCP_CLOSE_WAIT &&
            (rx_pcb->flags & TCP_PCB_FLAG_EOF) != 0) {
            klog("[TCP TEST] PASS: Receive Path (FIN recebido, transicao para CLOSE_WAIT e flag EOF)\n");
        } else {
            klog("[TCP TEST] FAIL: Receive Path FIN\n");
        }

        tcp_socket_destroy(rx_pcb);
    }

    /* Test 15: TX Buffer & Unacknowledged Queue operations */
    tcp_pcb_t *tx_pcb = tcp_alloc();
    if (tx_pcb != 0) {
        tx_pcb->local_ip = htonl(0x0A00020FU);
        tx_pcb->remote_ip = htonl(0x0A000202U);
        tx_pcb->local_port = 6666;
        tx_pcb->remote_port = 44444;
        tx_pcb->iss = 1000;
        tx_pcb->snd_una = 1001;
        tx_pcb->snd_nxt = 1001;
        tx_pcb->seq_number = 1001;
        tx_pcb->rcv_nxt = 2000;
        tx_pcb->ack_number = 2000;
        tx_pcb->state = TCP_ESTABLISHED;
        tcp_register(tx_pcb);

        /* Send 10 bytes: seq 1001..1011 */
        int s1 = tcp_send(tx_pcb, (const uint8_t *)"0123456789", 10);
        /* Send another 10 bytes: seq 1011..1021 */
        int s2 = tcp_send(tx_pcb, (const uint8_t *)"ABCDEFGHIJ", 10);

        int q_ok = (s1 == 10 && s2 == 10 && tx_pcb->snd_nxt == 1021 &&
                    tx_pcb->tx_buf.unsent.bytes == 0 &&
                    tx_pcb->tx_buf.unacked.bytes == 20 &&
                    tx_pcb->tx_buf.unacked.segments == 2);

        /* Test Invalid ACK (ack = 1050 > snd_nxt=1021): should be rejected */
        mutex_lock(&tx_pcb->lock);
        tcp_tx_ack_received_locked(tx_pcb, 1050);
        int inv_ok = (tx_pcb->snd_una == 1001 && tx_pcb->tx_buf.unacked.bytes == 20);

        /* Test Duplicate ACK (ack = 1001 == snd_una): should not advance */
        tcp_tx_ack_received_locked(tx_pcb, 1001);
        int dup_ok = (tx_pcb->snd_una == 1001 && tx_pcb->tx_buf.unacked.bytes == 20);

        /* Test Partial ACK (ack = 1006): acknowledges 5 bytes of segment 1 */
        tcp_tx_ack_received_locked(tx_pcb, 1006);
        int part_ok = (tx_pcb->snd_una == 1006 && tx_pcb->tx_buf.unacked.bytes == 15 &&
                       tx_pcb->tx_buf.unacked.head != 0 &&
                       tx_pcb->tx_buf.unacked.head->sequence == 1006 &&
                       tx_pcb->tx_buf.unacked.head->length == 5 &&
                       tx_pcb->tx_buf.unacked.head->offset == 5);

        /* Test Full ACK (ack = 1021): acknowledges all remaining bytes */
        tcp_tx_ack_received_locked(tx_pcb, 1021);
        int full_ok = (tx_pcb->snd_una == 1021 && tx_pcb->tx_buf.unacked.bytes == 0 &&
                       tx_pcb->tx_buf.unacked.head == 0 &&
                       (tx_pcb->flags & TCP_PCB_FLAG_TIMER_RTO) == 0);
        mutex_unlock(&tx_pcb->lock);

        if (q_ok && inv_ok && dup_ok && part_ok && full_ok) {
            klog("[TCP TEST] PASS: tcp_tx_buffer (enfileiramento, ACK parcial, cumulativo e validacao de limites)\n");
        } else {
            klog("[TCP TEST] FAIL: tcp_tx_buffer\n");
        }
        tcp_socket_destroy(tx_pcb);
    }

    /* Test 16: TX Retransmission & RTO ticks */
    tcp_pcb_t *rt_pcb = tcp_alloc();
    if (rt_pcb != 0) {
        rt_pcb->local_ip = htonl(0x0A00020FU);
        rt_pcb->remote_ip = htonl(0x0A000202U);
        rt_pcb->local_port = 6667;
        rt_pcb->remote_port = 44445;
        rt_pcb->iss = 5000;
        rt_pcb->snd_una = 5001;
        rt_pcb->snd_nxt = 5001;
        rt_pcb->seq_number = 5001;
        rt_pcb->rcv_nxt = 7000;
        rt_pcb->ack_number = 7000;
        rt_pcb->state = TCP_ESTABLISHED;
        tcp_register(rt_pcb);

        tcp_send(rt_pcb, (const uint8_t *)"RETRY_ME", 8);
        uint32_t first_rto = rt_pcb->timers.rto_ticks;

        /* Force tick to expire RTO */
        tcp_timer_tick(rt_pcb->timers.retransmission + 1);

        int rto_ok = (rt_pcb->timers.retransmit_count == 1 &&
                      rt_pcb->timers.rto_ticks == first_rto * 2 &&
                      rt_pcb->tx_buf.unacked.head != 0 &&
                      rt_pcb->tx_buf.unacked.head->sequence == 5001);

        /* Simulate reaching max retries */
        rt_pcb->timers.retransmit_count = TCP_MAX_DATA_RETRIES;
        tcp_timer_tick(rt_pcb->timers.retransmission + 1);
        int retry_limit_ok = (rt_pcb->state == TCP_CLOSED &&
                              (rt_pcb->flags & TCP_PCB_FLAG_RESET) != 0 &&
                              rt_pcb->tx_buf.unacked.head == 0);

        if (rto_ok && retry_limit_ok) {
            klog("[TCP TEST] PASS: tcp_tx_retransmission (RTO tick, backoff exponencial e limite de retries)\n");
        } else {
            klog("[TCP TEST] FAIL: tcp_tx_retransmission\n");
        }
        tcp_socket_destroy(rt_pcb);
    }

    klog("[TCP TEST] Finalizada Suite de Testes TCP.\n");
    klog("========================================\n\n");
}
