#include "net.h"

#include "e1000.h"
#include "scheduler.h"
#include "serial.h"
#include "mutex.h"
#include "vmm.h"
#include "heap.h"
#include "vfs.h"

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

#define SOCKET_RX_BUF_SIZE 16
#define RX_PACKET_DATA_SIZE 1500U
#define NET_IPV4_PAYLOAD_MAX 1480U
#define NET_USER_BASE 0x0000008000000000ULL
#define NET_PAGE_SIZE 4096ULL

struct udp_header {
    uint16_t src_port;
    uint16_t dest_port;
    uint16_t length;
    uint16_t checksum;
} __attribute__((packed));

struct socket_packet {
    uint8_t *data;
    size_t len;
};

struct socket {
    int fd;
    int domain;
    int type;
    int protocol;
    uint16_t local_port;
    uint32_t local_addr;
    
    struct socket_packet rx_queue[SOCKET_RX_BUF_SIZE];
    size_t rx_head;
    size_t rx_tail;
    size_t rx_count;
    
    int active;
};

#define MAX_SOCKETS 16
static struct socket sockets[MAX_SOCKETS];

static inline uint64_t save_and_disable_interrupts(void)
{
    uint64_t rflags;
    __asm__ volatile ("pushfq; popq %0" : "=r"(rflags));
    __asm__ volatile ("cli" ::: "memory");
    return rflags;
}

static inline void restore_interrupts(uint64_t rflags)
{
    if ((rflags & 0x200ULL) != 0) {
        __asm__ volatile ("sti" ::: "memory");
    }
}

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

static int net_validate_udp_checksum(struct ip_header *ip, struct udp_header *udp)
{
    uint16_t checksum = ntohs(udp->checksum);
    if (checksum == 0) {
        return 1;
    }

    uint32_t sum = 0;

    const uint8_t *src_bytes = (const uint8_t *)&ip->src_ip;
    sum += ((uint16_t)src_bytes[0] << 8) | src_bytes[1];
    sum += ((uint16_t)src_bytes[2] << 8) | src_bytes[3];

    const uint8_t *dest_bytes = (const uint8_t *)&ip->dest_ip;
    sum += ((uint16_t)dest_bytes[0] << 8) | dest_bytes[1];
    sum += ((uint16_t)dest_bytes[2] << 8) | dest_bytes[3];

    sum += (uint16_t)ip->protocol;
    sum += ntohs(udp->length);

    uint16_t udp_len = ntohs(udp->length);
    const uint8_t *bytes = (const uint8_t *)udp;
    size_t length = udp_len;

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

    return (uint16_t)~sum == 0;
}

int socket_vfs_read(vfs_node_t *node, uint64_t offset, uint32_t size, uint8_t *buffer)
{
    (void)offset;
    if (node == 0 || node->data == 0 || buffer == 0) {
        return -1;
    }

    struct socket *sock = (struct socket *)node->data;
    uint64_t rflags = save_and_disable_interrupts();

    if (sock->rx_count == 0) {
        restore_interrupts(rflags);
        return -11;
    }

    struct socket_packet *pkt = &sock->rx_queue[sock->rx_head];
    size_t copy_len = pkt->len;
    if (copy_len > size) {
        copy_len = size;
    }

    net_memcpy(buffer, pkt->data, copy_len);

    kfree(pkt->data);
    pkt->data = 0;

    sock->rx_head = (sock->rx_head + 1) % SOCKET_RX_BUF_SIZE;
    sock->rx_count--;

    restore_interrupts(rflags);

    return (int)copy_len;
}

size_t socket_vfs_write(vfs_node_t *node, size_t offset, size_t size, const uint8_t *buffer)
{
    (void)node; (void)offset; (void)size; (void)buffer;
    return 0;
}

void socket_vfs_close(vfs_node_t *node)
{
    if (node == 0) return;
    struct socket *sock = (struct socket *)node->data;
    if (sock != 0) {
        uint64_t rflags = save_and_disable_interrupts();
        sock->active = 0;
        for (size_t i = 0; i < sock->rx_count; i++) {
            size_t idx = (sock->rx_head + i) % SOCKET_RX_BUF_SIZE;
            if (sock->rx_queue[idx].data != 0) {
                kfree(sock->rx_queue[idx].data);
                sock->rx_queue[idx].data = 0;
            }
        }
        sock->rx_count = 0;
        restore_interrupts(rflags);
    }
    kfree(node);
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
        if ((size_t)length >= sizeof(struct eth_header) + sizeof(struct ip_header)) {
            struct ip_header *ip = (struct ip_header *)(frame + sizeof(struct eth_header));
            size_t ip_header_length = (size_t)(ip->ver_ihl & 0x0FU) * 4U;
            uint16_t total_length = ntohs(ip->total_length);

            if ((ip->ver_ihl >> 4) == 4U &&
                ip_header_length >= sizeof(struct ip_header) &&
                total_length >= ip_header_length &&
                (size_t)length >= sizeof(struct eth_header) + total_length &&
                net_is_local_ipv4(ip->dest_ip)) {

                if (net_checksum(ip, ip_header_length) != 0) {
                    klog("NET: Checksum IP invalido.\n");
                    return;
                }

                uint8_t *payload = (uint8_t *)ip + ip_header_length;
                size_t payload_len = (size_t)total_length - ip_header_length;

                if (ip->protocol == IP_PROTO_ICMP) {
                    if (payload_len < sizeof(struct icmp_packet)) {
                        return;
                    }
                    struct icmp_packet *icmp = (struct icmp_packet *)payload;
                    if (net_checksum(icmp, payload_len) != 0) {
                        klog("NET: Checksum ICMP invalido.\n");
                        return;
                    }
                    
                    net_handle_icmp(frame, (size_t)length);
                }
                else if (ip->protocol == IP_PROTO_UDP) {
                    if (payload_len < sizeof(struct udp_header)) {
                        return;
                    }
                    struct udp_header *udp = (struct udp_header *)payload;
                    if (!net_validate_udp_checksum(ip, udp)) {
                        klog("NET: Checksum UDP invalido.\n");
                        return;
                    }
                }

                uint64_t rflags = save_and_disable_interrupts();
                int dispatched = 0;
                for (int i = 0; i < MAX_SOCKETS; i++) {
                    struct socket *sock = &sockets[i];
                    if (!sock->active) {
                        continue;
                    }

                    int match = 0;
                    size_t route_len = 0;
                    uint8_t *route_data = 0;

                    if (sock->type == SOCK_RAW && sock->protocol == ip->protocol) {
                        match = 1;
                        route_len = payload_len;
                        route_data = payload;
                    }
                    else if (sock->type == SOCK_DGRAM && ip->protocol == IP_PROTO_UDP) {
                        struct udp_header *udp = (struct udp_header *)payload;
                        if (sock->local_port == ntohs(udp->dest_port)) {
                            match = 1;
                            route_len = payload_len - sizeof(struct udp_header);
                            route_data = payload + sizeof(struct udp_header);
                        }
                    }

                    if (match) {
                        dispatched = 1;
                        if (sock->rx_count >= SOCKET_RX_BUF_SIZE) {
                            klog("NET: Buffer do socket cheio, descartando pacote.\n");
                            continue;
                        }

                        uint8_t *pkt_buf = kmalloc(route_len);
                        if (pkt_buf == 0) {
                            klog("NET: Falha de kmalloc ao enfileirar pacote.\n");
                            continue;
                        }

                        net_memcpy(pkt_buf, route_data, route_len);

                        size_t tail = sock->rx_tail;
                        sock->rx_queue[tail].data = pkt_buf;
                        sock->rx_queue[tail].len = route_len;

                        sock->rx_tail = (tail + 1) % SOCKET_RX_BUF_SIZE;
                        sock->rx_count++;

                        scheduler_wake_socket_receivers(sock->protocol);
                    }
                }

                if (!dispatched) {
                    klog("NET: Pacote descartado por falta de socket ativo.\n");
                }

                restore_interrupts(rflags);
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

    struct socket *sock = 0;
    for (int i = 0; i < MAX_SOCKETS; i++) {
        if (sockets[i].active && sockets[i].protocol == protocol) {
            sock = &sockets[i];
            break;
        }
    }

    if (sock == 0) {
        return -1;
    }

    for (int attempts = 0; attempts < 10000; attempts++) {
        net_poll_packets();

        uint64_t rflags = save_and_disable_interrupts();
        if (sock->rx_count > 0) {
            struct socket_packet *pkt = &sock->rx_queue[sock->rx_head];
            size_t copy_len = pkt->len;
            if (copy_len > max_len) {
                copy_len = max_len;
            }
            net_memcpy(buffer, pkt->data, copy_len);
            kfree(pkt->data);
            pkt->data = 0;
            sock->rx_head = (sock->rx_head + 1) % SOCKET_RX_BUF_SIZE;
            sock->rx_count--;
            restore_interrupts(rflags);
            klog("NET: Syscall socket_recv recebeu pacote.\n");
            return (int)copy_len;
        }
        restore_interrupts(rflags);

        scheduler_sleep_current(TASK_WAIT_SOCKET_RECV, (uint64_t)protocol);
        scheduler_yield();
    }

    return 0;
}

int sys_socket(int domain, int type, int protocol)
{
    if (domain != AF_INET) {
        return -1;
    }
    if (type != SOCK_RAW && type != SOCK_DGRAM) {
        return -1;
    }

    uint64_t rflags = save_and_disable_interrupts();
    int slot = -1;
    for (int i = 0; i < MAX_SOCKETS; i++) {
        if (!sockets[i].active) {
            slot = i;
            break;
        }
    }

    if (slot < 0) {
        restore_interrupts(rflags);
        return -1;
    }

    struct socket *sock = &sockets[slot];
    sock->domain = domain;
    sock->type = type;
    sock->protocol = protocol;
    sock->local_port = 0;
    sock->local_addr = 0;
    sock->rx_head = 0;
    sock->rx_tail = 0;
    sock->rx_count = 0;
    for (int j = 0; j < SOCKET_RX_BUF_SIZE; j++) {
        sock->rx_queue[j].data = 0;
        sock->rx_queue[j].len = 0;
    }
    sock->active = 1;

    restore_interrupts(rflags);

    vfs_node_t *node = kmalloc(sizeof(vfs_node_t));
    if (node == 0) {
        uint64_t rflags2 = save_and_disable_interrupts();
        sock->active = 0;
        restore_interrupts(rflags2);
        return -1;
    }

    for (int j = 0; j < VFS_NAME_MAX; j++) {
        node->name[j] = 0;
    }
    node->name[0] = 's';
    node->type = VFS_NODE_DEVICE;
    node->data = sock;
    node->read = socket_vfs_read;
    node->write = socket_vfs_write;
    node->close = socket_vfs_close;

    task_t *task = scheduler_current_task();
    int fd = task_alloc_fd(task, node);
    if (fd < 0) {
        kfree(node);
        uint64_t rflags2 = save_and_disable_interrupts();
        sock->active = 0;
        restore_interrupts(rflags2);
        return -1;
    }

    sock->fd = fd;
    return fd;
}

int sys_bind(int fd, const struct sockaddr *addr, uint32_t addrlen)
{
    task_t *task = scheduler_current_task();
    if (task == 0 || fd < 0 || fd >= TASK_MAX_FDS) {
        return -1;
    }
    vfs_node_t *node = task->file_descriptors[fd];
    if (node == 0 || node->read != socket_vfs_read) {
        return -1;
    }
    if (addr == 0 || addrlen < sizeof(struct sockaddr_in)) {
        return -1;
    }
    if (!vmm_is_mapped((uint64_t *)task->cr3, (uintptr_t)addr)) {
        return -1;
    }

    struct sockaddr_in addr_in;
    net_memcpy(&addr_in, addr, sizeof(struct sockaddr_in));

    struct socket *sock = (struct socket *)node->data;
    if (sock == 0 || !sock->active) {
        return -1;
    }

    sock->local_port = ntohs(addr_in.sin_port);
    sock->local_addr = addr_in.sin_addr.s_addr;
    return 0;
}
