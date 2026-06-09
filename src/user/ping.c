#include <stddef.h>
#include <stdint.h>

#include "sys/socket.h"
#include "ulibc.h"

#define PING_DATA_SIZE 56U
#define PING_PACKET_SIZE (sizeof(struct icmp_packet) + PING_DATA_SIZE)
#define PING_IDENTIFIER 0x5048U
#define PING_REQUEST_COUNT 4
#define PING_TIMEOUT_TICKS 1000
#define PING_INTERVAL_TICKS 1000

struct __attribute__((packed)) ping_echo_packet {
    struct icmp_packet icmp;
    uint8_t data[PING_DATA_SIZE];
};

static uint16_t ping_checksum(const void *data, size_t length)
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

static const char *skip_spaces(const char *str)
{
    while (str != 0 && *str == ' ') {
        str++;
    }

    return str;
}

static int copy_target(char *dest, size_t capacity, const char *arg)
{
    size_t len;

    if (dest == 0 || capacity == 0 || arg == 0) {
        return -1;
    }

    arg = skip_spaces(arg);
    len = strlen(arg);
    while (len > 0 && arg[len - 1] == ' ') {
        len--;
    }

    if (len == 0 || len >= capacity) {
        return -1;
    }

    memcpy(dest, arg, len);
    dest[len] = '\0';
    return 0;
}

static void fill_ping_payload(struct ping_echo_packet *packet, uint16_t sequence)
{
    packet->icmp.type = ICMP_TYPE_ECHO_REQ;
    packet->icmp.code = 0;
    packet->icmp.checksum = 0;
    packet->icmp.id = htons(PING_IDENTIFIER);
    packet->icmp.sequence = htons(sequence);

    for (size_t i = 0; i < sizeof(packet->data); i++) {
        packet->data[i] = (uint8_t)('A' + (i % 26U));
    }

    packet->icmp.checksum = htons(ping_checksum(packet, sizeof(*packet)));
}

static void ping_delay_ticks(int ticks)
{
    for (int i = 0; i < ticks; i++) {
        yield();
    }
}

static int is_matching_echo_reply(const uint8_t *buffer, int length,
    const struct ping_echo_packet *request)
{
    if (length < (int)sizeof(struct icmp_packet)) {
        return 0;
    }

    const struct icmp_packet *reply = (const struct icmp_packet *)buffer;
    return reply->type == ICMP_TYPE_ECHO_REPLY &&
        reply->code == 0 &&
        reply->id == request->icmp.id &&
        reply->sequence == request->icmp.sequence;
}

void _start(const char *arg)
{
    char target_text[32];
    uint32_t target_ip;
    struct ping_echo_packet request;
    uint8_t reply_buffer[1500];
    unsigned int transmitted = 0;
    unsigned int received_count = 0;
    uint64_t min_rtt = (uint64_t)-1;
    uint64_t max_rtt = 0;
    uint64_t total_rtt = 0;

    if (copy_target(target_text, sizeof(target_text), arg) != 0) {
        printf("uso: ping <endereco_ip>\n");
        exit(1);
    }

    target_ip = inet_addr(target_text);
    if (target_ip == 0) {
        printf("ping: endereco IP invalido: %s\n", target_text);
        exit(1);
    }

    printf("A disparar ping contra %s com %u bytes de dados...\n",
        target_text, (unsigned int)PING_PACKET_SIZE);

    for (int i = 0; i < PING_REQUEST_COUNT; i++) {
        fill_ping_payload(&request, (uint16_t)(i + 1));
        transmitted++;

        uint64_t start_ticks = get_ticks();
        printf("[SYS] Socket enviado em Ring 3. Payload de %u bytes transmitido.\n",
            (unsigned int)PING_PACKET_SIZE);

        if (socket_send(target_ip, IP_PROTO_ICMP, &request, sizeof(request)) < 0) {
            printf("ping: falha ao enviar pacote ICMP seq=%u\n",
                (unsigned int)(i + 1));
            ping_delay_ticks(PING_INTERVAL_TICKS);
            continue;
        }

        int got_reply = 0;
        for (int timeout_ticks = 0; timeout_ticks < PING_TIMEOUT_TICKS;
            timeout_ticks++) {
            int bytes = socket_recv(IP_PROTO_ICMP, reply_buffer,
                sizeof(reply_buffer));

            if (bytes > 0 &&
                is_matching_echo_reply(reply_buffer, bytes, &request)) {
                uint64_t end_ticks = get_ticks();
                uint64_t delta_ticks = end_ticks - start_ticks;
                
                struct icmp_packet *reply =
                    (struct icmp_packet *)reply_buffer;
                const char *checksum_state =
                    ping_checksum(reply_buffer, (size_t)bytes) == 0 ?
                        "ok" : "invalido";

                if (delta_ticks < min_rtt) {
                    min_rtt = delta_ticks;
                }
                if (delta_ticks > max_rtt) {
                    max_rtt = delta_ticks;
                }
                total_rtt += delta_ticks;

                printf("[RX] %u bytes de %s: seq=%u ttl=64 rtt=%u ms (Kernel: +%u ticks)\n",
                    (unsigned int)bytes, target_text,
                    (unsigned int)ntohs(reply->sequence), 
                    (unsigned int)delta_ticks, (unsigned int)delta_ticks);
                printf("     checksum=%s\n", checksum_state);
                received_count++;
                got_reply = 1;
                break;
            }

            yield();
        }

        if (!got_reply) {
            printf("Timeout aguardando resposta de %s: icmp_seq=%u\n",
                target_text, (unsigned int)(i + 1));
        }

        if (i + 1 < PING_REQUEST_COUNT) {
            ping_delay_ticks(PING_INTERVAL_TICKS);
        }
    }

    unsigned int lost = transmitted - received_count;
    unsigned int loss_percent = transmitted == 0 ? 0 :
        (lost * 100U) / transmitted;

    printf("--- %s ping estatisticas ---\n", target_text);
    printf("%u pacotes transmitidos, %u recebidos, %u%% perda de pacotes\n",
        transmitted, received_count, loss_percent);
    
    if (received_count > 0) {
        uint64_t avg_rtt = total_rtt / received_count;
        printf("Tempo de ida e volta (RTT) min/avg/max = %u/%u/%u ms\n",
            (unsigned int)min_rtt, (unsigned int)avg_rtt, (unsigned int)max_rtt);
    }
    
    exit(received_count == 0 ? 1 : 0);
}
