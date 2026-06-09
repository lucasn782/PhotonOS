#ifndef PHOTONOS_SYS_SOCKET_H
#define PHOTONOS_SYS_SOCKET_H

#include <stddef.h>
#include <stdint.h>

#define IP_PROTO_ICMP 1U

#define ICMP_TYPE_ECHO_REPLY 0U
#define ICMP_TYPE_ECHO_REQ   8U

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

int socket_send(uint32_t dest_ip, uint8_t protocol, const void *payload,
    size_t len);
int socket_recv(uint8_t protocol, void *buffer, size_t max_len);
uint32_t inet_addr(const char *ip_str);

#endif
