#include "net.h"

#include "e1000.h"
#include "scheduler.h"
#include "serial.h"

#define NET_LOCAL_IPV4 0x0A00020FU

static const uint8_t net_local_mac[NET_ETH_ADDR_LEN] = {
    0x52, 0x54, 0x00, 0x12, 0x34, 0x56
};

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
        arp->proto_len != NET_IPV4_ADDR_LEN ||
        ntohs(arp->opcode) != ARP_OPCODE_REQUEST ||
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
