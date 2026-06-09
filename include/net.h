#ifndef PHOTONOS_NET_H
#define PHOTONOS_NET_H

#include <stddef.h>
#include <stdint.h>

#define ETH_TYPE_ARP  0x0806U
#define ETH_TYPE_IPV4 0x0800U

#define ICMP_TYPE_ECHO_REQ   8U
#define ICMP_TYPE_ECHO_REPLY 0U

#define IP_PROTO_ICMP 1U

#define ARP_HW_TYPE_ETHERNET 1U
#define ARP_OPCODE_REQUEST 1U
#define ARP_OPCODE_REPLY 2U

#define NET_ETH_ADDR_LEN 6U
#define NET_IPV4_ADDR_LEN 4U
#define NET_MTU_FRAME_SIZE 1518U

struct __attribute__((packed)) eth_header {
    uint8_t dest_mac[NET_ETH_ADDR_LEN];
    uint8_t src_mac[NET_ETH_ADDR_LEN];
    uint16_t ethertype;
};

struct __attribute__((packed)) arp_packet {
    uint16_t hw_type;
    uint16_t proto_type;
    uint8_t hw_len;
    uint8_t proto_len;
    uint16_t opcode;
    uint8_t src_mac[NET_ETH_ADDR_LEN];
    uint32_t src_ip;
    uint8_t dest_mac[NET_ETH_ADDR_LEN];
    uint32_t dest_ip;
};

struct __attribute__((packed)) ip_header {
    uint8_t ver_ihl;
    uint8_t tos;
    uint16_t total_length;
    uint16_t id;
    uint16_t flags_fragment;
    uint8_t ttl;
    uint8_t protocol;
    uint16_t checksum;
    uint32_t src_ip;
    uint32_t dest_ip;
};

struct __attribute__((packed)) icmp_packet {
    uint8_t type;
    uint8_t code;
    uint16_t checksum;
    uint16_t id;
    uint16_t sequence;
};

static inline uint16_t htons(uint16_t value)
{
    return (uint16_t)((value << 8) | (value >> 8));
}

static inline uint16_t ntohs(uint16_t value)
{
    return htons(value);
}

static inline uint32_t htonl(uint32_t value)
{
    return ((value & 0x000000FFU) << 24) |
        ((value & 0x0000FF00U) << 8) |
        ((value & 0x00FF0000U) >> 8) |
        ((value & 0xFF000000U) >> 24);
}

static inline uint32_t ntohl(uint32_t value)
{
    return htonl(value);
}

void net_poll_packets(void);
void net_handle_arp(uint8_t *frame, size_t frame_length);
void net_handle_icmp(uint8_t *frame, size_t frame_length);
void net_kernel_thread(void);

int sys_socket_send(uint32_t dest_ip, uint8_t protocol, const void *payload, size_t len);
int sys_socket_recv(uint8_t protocol, void *buffer, size_t max_len);

#endif
