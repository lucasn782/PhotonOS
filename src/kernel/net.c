#include "net.h"

#include "e1000.h"
#include "scheduler.h"
#include "serial.h"
#include "mutex.h"
#include "vmm.h"

#define NET_LOCAL_IPV4 0x0A00020FU

static const uint8_t net_local_mac[NET_ETH_ADDR_LEN] = {
    0x52, 0x54, 0x00, 0x12, 0x34, 0x56
};

struct arp_entry {
    uint32_t ip;
    uint8_t mac[NET_ETH_ADDR_LEN];
    int resolved;
};

#define ARP_CACHE_SIZE 16
static struct arp_entry arp_cache[ARP_CACHE_SIZE];
static size_t arp_cache_count = 0;

#define RX_QUEUE_SIZE 16
#define RX_PACKET_DATA_SIZE 1500U
#define NET_IPV4_PAYLOAD_MAX 1480U
#define NET_USER_BASE 0x0000008000000000ULL
#define NET_PAGE_SIZE 4096ULL

struct rx_packet {
    uint8_t protocol;
    uint8_t data[RX_PACKET_DATA_SIZE];
    size_t len;
};

static struct rx_packet rx_queue[RX_QUEUE_SIZE];
static size_t rx_queue_head = 0;
static size_t rx_queue_tail = 0;
static size_t rx_queue_count = 0;
static mutex_t rx_queue_mutex = {0};

static void net_memcpy(void *dest, const void *src, size_t count)
{
    uint8_t *out = dest;
    const uint8_t *in = src;

    for (size_t i = 0; i < count; i++) {
        out[i] = in[i];
    }
}

static void net_copy_mac(uint8_t *dest, const uint8_t *src)
{
    net_memcpy(dest, src, NET_ETH_ADDR_LEN);
}

static uint16_t net_checksum(const void *data, size_t length)
{
    const uint8_t *bytes = data;
    uint32_t sum = 0;

    while (length > 1) {
        sum += ((uint16_t)bytes[0] << 8) | bytes[1];
        bytes += 2;
        length -= 2;
    }

    if (length != 0) {
        sum += (uint16_t)bytes[0] << 8;
    }

    while ((sum >> 16) != 0) {
        sum = (sum & 0xFFFFU) + (sum >> 16);
    }

    return (uint16_t)~sum;
}

static int net_is_local_ipv4(uint32_t ip)
{
    return ip == htonl(NET_LOCAL_IPV4);
}

static int user_buffer_accessible(const void *buffer, size_t len)
{
    task_t *task = scheduler_current_task();
    uintptr_t start = (uintptr_t)buffer;
    uintptr_t end;
    uintptr_t page;
    uintptr_t last_page;

    if (len == 0) {
        return 1;
    }

    if (task == 0 || task->cr3 == (uint64_t)vmm_kernel_pml4() ||
        start < NET_USER_BASE) {
        return 0;
    }

    end = start + len - 1U;
    if (end < start) {
        return 0;
    }

    page = start & ~(NET_PAGE_SIZE - 1ULL);
    last_page = end & ~(NET_PAGE_SIZE - 1ULL);
    for (;;) {
        if (!vmm_is_mapped((uint64_t *)task->cr3, page)) {
            return 0;
        }
        if (page == last_page) {
            break;
        }
        page += NET_PAGE_SIZE;
    }

    return 1;
}

static void arp_cache_update(uint32_t ip, const uint8_t *mac)
{
    for (size_t i = 0; i < arp_cache_count; i++) {
        if (arp_cache[i].ip == ip) {
            net_copy_mac(arp_cache[i].mac, mac);
            arp_cache[i].resolved = 1;
            return;
        }
    }

    if (arp_cache_count < ARP_CACHE_SIZE) {
        arp_cache[arp_cache_count].ip = ip;
        net_copy_mac(arp_cache[arp_cache_count].mac, mac);
        arp_cache[arp_cache_count].resolved = 1;
        arp_cache_count++;
    } else {
        for (size_t i = 1; i < ARP_CACHE_SIZE; i++) {
            arp_cache[i - 1] = arp_cache[i];
        }
        arp_cache[ARP_CACHE_SIZE - 1].ip = ip;
        net_copy_mac(arp_cache[ARP_CACHE_SIZE - 1].mac, mac);
        arp_cache[ARP_CACHE_SIZE - 1].resolved = 1;
    }
}

static int arp_resolve(uint32_t ip, uint8_t *out_mac)
{
    for (size_t i = 0; i < arp_cache_count; i++) {
        if (arp_cache[i].ip == ip && arp_cache[i].resolved) {
            net_copy_mac(out_mac, arp_cache[i].mac);
            return 1;
        }
    }
    return 0;
}

static void arp_send_request(uint32_t ip)
{
    uint8_t frame[sizeof(struct eth_header) + sizeof(struct arp_packet)];
    struct eth_header *eth = (struct eth_header *)frame;
    struct arp_packet *arp = (struct arp_packet *)(frame + sizeof(struct eth_header));

    for (size_t i = 0; i < NET_ETH_ADDR_LEN; i++) {
        eth->dest_mac[i] = 0xFF;
    }
    net_copy_mac(eth->src_mac, net_local_mac);
    eth->ethertype = htons(ETH_TYPE_ARP);

    arp->hw_type = htons(ARP_HW_TYPE_ETHERNET);
    arp->proto_type = htons(ETH_TYPE_IPV4);
    arp->hw_len = NET_ETH_ADDR_LEN;
    arp->proto_len = NET_IPV4_ADDR_LEN;
    arp->opcode = htons(ARP_OPCODE_REQUEST);
    net_copy_mac(arp->src_mac, net_local_mac);
    arp->src_ip = htonl(NET_LOCAL_IPV4);
    for (size_t i = 0; i < NET_ETH_ADDR_LEN; i++) {
        arp->dest_mac[i] = 0x00;
    }
    arp->dest_ip = ip;

    e1000_send_packet(frame, sizeof(frame));
}

static void rx_queue_enqueue(uint8_t protocol, const uint8_t *data, size_t len)
{
    mutex_lock(&rx_queue_mutex);

    if (rx_queue_count == RX_QUEUE_SIZE) {
        rx_queue_head = (rx_queue_head + 1) % RX_QUEUE_SIZE;
        rx_queue_count--;
    }

    struct rx_packet *pkt = &rx_queue[rx_queue_tail];
    pkt->protocol = protocol;
    pkt->len = len > RX_PACKET_DATA_SIZE ? RX_PACKET_DATA_SIZE : len;
    for (size_t i = 0; i < pkt->len; i++) {
        pkt->data[i] = data[i];
    }

    rx_queue_tail = (rx_queue_tail + 1) % RX_QUEUE_SIZE;
    rx_queue_count++;

    mutex_unlock(&rx_queue_mutex);
    scheduler_wake_socket_receivers(protocol);
}

static int rx_queue_dequeue(uint8_t protocol, void *buffer, size_t max_len,
    size_t *out_len)
{
    mutex_lock(&rx_queue_mutex);

    for (size_t i = 0; i < rx_queue_count; i++) {
        size_t idx = (rx_queue_head + i) % RX_QUEUE_SIZE;
        if (rx_queue[idx].protocol == protocol) {
            size_t copy_len = rx_queue[idx].len;
            if (copy_len > max_len) {
                copy_len = max_len;
            }

            for (size_t j = 0; j < copy_len; j++) {
                ((uint8_t *)buffer)[j] = rx_queue[idx].data[j];
            }
            *out_len = copy_len;

            size_t target_idx = idx;
            size_t next_logical = i + 1;
            while (next_logical < rx_queue_count) {
                size_t next_idx = (rx_queue_head + next_logical) % RX_QUEUE_SIZE;
                rx_queue[target_idx] = rx_queue[next_idx];
                target_idx = next_idx;
                next_logical++;
            }
            rx_queue_tail = (rx_queue_tail + RX_QUEUE_SIZE - 1) % RX_QUEUE_SIZE;
            rx_queue_count--;
            mutex_unlock(&rx_queue_mutex);
            return 1;
        }
    }

    mutex_unlock(&rx_queue_mutex);
    return 0;
}

void net_handle_arp(uint8_t *frame, size_t frame_length)
{
    if (frame == 0 ||
        frame_length < sizeof(struct eth_header) + sizeof(struct arp_packet)) {
        return;
    }

    struct eth_header *eth = (struct eth_header *)frame;
    struct arp_packet *arp =
        (struct arp_packet *)(frame + sizeof(struct eth_header));

    if (ntohs(arp->hw_type) != ARP_HW_TYPE_ETHERNET ||
        ntohs(arp->proto_type) != ETH_TYPE_IPV4 ||
        arp->hw_len != NET_ETH_ADDR_LEN ||
        arp->proto_len != NET_IPV4_ADDR_LEN) {
        return;
    }

    arp_cache_update(arp->src_ip, arp->src_mac);

    if (ntohs(arp->opcode) != ARP_OPCODE_REQUEST ||
        !net_is_local_ipv4(arp->dest_ip)) {
        return;
    }

    uint8_t reply[sizeof(struct eth_header) + sizeof(struct arp_packet)];
    struct eth_header *reply_eth = (struct eth_header *)reply;
    struct arp_packet *reply_arp =
        (struct arp_packet *)(reply + sizeof(struct eth_header));

    net_copy_mac(reply_eth->dest_mac, eth->src_mac);
    net_copy_mac(reply_eth->src_mac, net_local_mac);
    reply_eth->ethertype = htons(ETH_TYPE_ARP);

    reply_arp->hw_type = htons(ARP_HW_TYPE_ETHERNET);
    reply_arp->proto_type = htons(ETH_TYPE_IPV4);
    reply_arp->hw_len = NET_ETH_ADDR_LEN;
    reply_arp->proto_len = NET_IPV4_ADDR_LEN;
    reply_arp->opcode = htons(ARP_OPCODE_REPLY);
    net_copy_mac(reply_arp->src_mac, net_local_mac);
    reply_arp->src_ip = htonl(NET_LOCAL_IPV4);
    net_copy_mac(reply_arp->dest_mac, arp->src_mac);
    reply_arp->dest_ip = arp->src_ip;

    if (e1000_send_packet(reply, sizeof(reply)) > 0) {
        klog("NET: Resposta ARP enviada.\n");
    }
}

void net_handle_icmp(uint8_t *frame, size_t frame_length)
{
    if (frame == 0 ||
        frame_length < sizeof(struct eth_header) + sizeof(struct ip_header)) {
        return;
    }

    struct eth_header *eth = (struct eth_header *)frame;
    struct ip_header *ip =
        (struct ip_header *)(frame + sizeof(struct eth_header));
    size_t ip_header_length = (size_t)(ip->ver_ihl & 0x0FU) * 4U;
    uint16_t total_length = ntohs(ip->total_length);

    if ((ip->ver_ihl >> 4) != 4U ||
        ip_header_length < sizeof(struct ip_header) ||
        total_length < ip_header_length + sizeof(struct icmp_packet) ||
        frame_length < sizeof(struct eth_header) + total_length ||
        ip->protocol != IP_PROTO_ICMP ||
        !net_is_local_ipv4(ip->dest_ip)) {
        return;
    }

    struct icmp_packet *icmp =
        (struct icmp_packet *)((uint8_t *)ip + ip_header_length);
    size_t icmp_length = (size_t)total_length - ip_header_length;

    if (icmp->type != ICMP_TYPE_ECHO_REQ || icmp->code != 0) {
        return;
    }

    uint8_t source_mac[NET_ETH_ADDR_LEN];
    uint32_t source_ip = ip->src_ip;

    net_copy_mac(source_mac, eth->src_mac);
    net_copy_mac(eth->dest_mac, source_mac);
    net_copy_mac(eth->src_mac, net_local_mac);

    ip->src_ip = htonl(NET_LOCAL_IPV4);
    ip->dest_ip = source_ip;
    ip->checksum = 0;
    ip->checksum = htons(net_checksum(ip, ip_header_length));

    icmp->type = ICMP_TYPE_ECHO_REPLY;
    icmp->checksum = 0;
    icmp->checksum = htons(net_checksum(icmp, icmp_length));

    if (e1000_send_packet(frame, sizeof(struct eth_header) + total_length) > 0) {
        klog("NET: Resposta ICMP Echo Reply enviada.\n");
    }
}

void net_poll_packets(void)
{
    static uint8_t frame[NET_MTU_FRAME_SIZE];
    int length = e1000_receive_packet(frame, sizeof(frame));

    if (length <= 0 || (size_t)length < sizeof(struct eth_header)) {
        return;
    }

    struct eth_header *eth = (struct eth_header *)frame;
    uint16_t ethertype = ntohs(eth->ethertype);

    if (ethertype == ETH_TYPE_ARP) {
        net_handle_arp(frame, (size_t)length);
    } else if (ethertype == ETH_TYPE_IPV4) {
        net_handle_icmp(frame, (size_t)length);

        if ((size_t)length >= sizeof(struct eth_header) + sizeof(struct ip_header)) {
            struct ip_header *ip = (struct ip_header *)(frame + sizeof(struct eth_header));
            size_t ip_header_length = (size_t)(ip->ver_ihl & 0x0FU) * 4U;
            uint16_t total_length = ntohs(ip->total_length);

            if ((ip->ver_ihl >> 4) == 4U &&
                ip_header_length >= sizeof(struct ip_header) &&
                total_length >= ip_header_length &&
                (size_t)length >= sizeof(struct eth_header) + total_length &&
                net_is_local_ipv4(ip->dest_ip)) {

                uint8_t *payload = (uint8_t *)ip + ip_header_length;
                size_t payload_len = (size_t)total_length - ip_header_length;
                rx_queue_enqueue(ip->protocol, payload, payload_len);
            }
        }
    }
}

void net_kernel_thread(void)
{
    klog("NET: Thread de rede em background iniciada.\n");

    for (;;) {
        net_poll_packets();
        scheduler_yield();
    }
}

int sys_socket_send(uint32_t dest_ip, uint8_t protocol, const void *payload, size_t len)
{
    klog("NET: Syscall socket_send invocada.\n");

    if (payload == 0 || len > NET_IPV4_PAYLOAD_MAX ||
        !user_buffer_accessible(payload, len)) {
        return -1;
    }

    uint8_t dest_mac[NET_ETH_ADDR_LEN];
    int resolved = 0;

    if (dest_ip == 0xFFFFFFFFU) {
        for (size_t i = 0; i < NET_ETH_ADDR_LEN; i++) {
            dest_mac[i] = 0xFF;
        }
        resolved = 1;
    } else {
        for (int attempts = 0; attempts < 1000; attempts++) {
            net_poll_packets();
            if (arp_resolve(dest_ip, dest_mac)) {
                resolved = 1;
                break;
            }
            if (attempts % 100 == 0) {
                arp_send_request(dest_ip);
            }
            scheduler_yield();
        }
    }

    if (!resolved) {
        klog("NET: Falha ao resolver MAC por ARP.\n");
        return -1;
    }

    static uint8_t frame[1600];
    struct eth_header *eth = (struct eth_header *)frame;
    struct ip_header *ip = (struct ip_header *)(frame + sizeof(struct eth_header));
    uint8_t *ip_payload = frame + sizeof(struct eth_header) + sizeof(struct ip_header);

    for (size_t i = 0; i < len; i++) {
        ip_payload[i] = ((const uint8_t *)payload)[i];
    }

    net_copy_mac(eth->dest_mac, dest_mac);
    net_copy_mac(eth->src_mac, net_local_mac);
    eth->ethertype = htons(ETH_TYPE_IPV4);

    ip->ver_ihl = 0x45;
    ip->tos = 0;
    ip->total_length = htons(sizeof(struct ip_header) + len);
    static uint16_t ip_id = 0;
    ip->id = htons(ip_id++);
    ip->flags_fragment = 0;
    ip->ttl = 64;
    ip->protocol = protocol;
    ip->src_ip = htonl(NET_LOCAL_IPV4);
    ip->dest_ip = dest_ip;
    ip->checksum = 0;
    ip->checksum = htons(net_checksum(ip, sizeof(struct ip_header)));

    int sent = e1000_send_packet(frame, sizeof(struct eth_header) + sizeof(struct ip_header) + len);
    if (sent > 0) {
        klog("NET: Syscall socket_send enviou pacote.\n");
        return 0;
    }

    klog("NET: Falha ao enviar pacote via e1000.\n");
    return -1;
}

int sys_socket_recv(uint8_t protocol, void *buffer, size_t max_len)
{
    klog("NET: Syscall socket_recv invocada.\n");

    if (buffer == 0) {
        return -1;
    }

    if (max_len > RX_PACKET_DATA_SIZE) {
        max_len = RX_PACKET_DATA_SIZE;
    }
    if (max_len == 0 || !user_buffer_accessible(buffer, max_len)) {
        return -1;
    }

    size_t match_len = 0;

    // Loop para aguardar pacote com bloqueio/despertar
    for (int attempts = 0; attempts < 10000; attempts++) {
        net_poll_packets();

        if (rx_queue_dequeue(protocol, buffer, max_len, &match_len)) {
            klog("NET: Syscall socket_recv recebeu pacote.\n");
            return (int)match_len;
        }

        // Bloqueia tarefa aguardando pacote e cede controle
        scheduler_sleep_current(TASK_WAIT_SOCKET_RECV, (uint64_t)protocol);
        scheduler_yield();
    }

    return 0;
}
