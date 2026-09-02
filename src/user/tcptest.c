#include <stddef.h>
#include <stdint.h>

#include "sys/socket.h"
#include "ulibc.h"

static char arg_copy[256];

static int parse_uint(const char *str, unsigned int *out)
{
    unsigned int val = 0;
    int saw_digit = 0;

    if (str == 0) {
        return -1;
    }
    while (*str == ' ') {
        str++;
    }
    while (*str >= '0' && *str <= '9') {
        saw_digit = 1;
        val = (val * 10U) + (unsigned int)(*str - '0');
        str++;
    }
    if (!saw_digit) {
        return -1;
    }
    *out = val;
    return 0;
}

static int do_connect_test(const char *ip_str, uint16_t port, int expect_success)
{
    uint32_t target_ip = inet_addr(ip_str);
    if (target_ip == 0) {
        printf("[TCPTEST] Endereco IP invalido: %s\n", ip_str);
        return -1;
    }

    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        printf("[TCPTEST] Falha em socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)\n");
        return -1;
    }

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = target_ip;
    for (int i = 0; i < 8; i++) addr.sin_zero[i] = 0;

    uint64_t t0 = get_ticks();
    int res = connect(fd, (const struct sockaddr *)&addr, sizeof(addr));
    uint64_t t1 = get_ticks();
    uint64_t rtt = t1 - t0;

    if (res == 0) {
        if (expect_success) {
            printf("[TCPTEST] PASS: connect(%s:%u) OK (3-Way Handshake ESTABLISHED, RTT: %u ticks)\n",
                ip_str, (unsigned int)port, (unsigned int)rtt);
        } else {
            printf("[TCPTEST] FAIL: connect(%s:%u) deveria ter falhado mas retornou 0\n",
                ip_str, (unsigned int)port);
        }
        close(fd);
        return expect_success ? 0 : -1;
    } else {
        if (!expect_success) {
            printf("[TCPTEST] PASS: connect(%s:%u) falhou como esperado (res=%d, decorrido: %u ticks)\n",
                ip_str, (unsigned int)port, res, (unsigned int)rtt);
        } else {
            printf("[TCPTEST] FAIL: connect(%s:%u) falhou (res=%d, decorrido: %u ticks)\n",
                ip_str, (unsigned int)port, res, (unsigned int)rtt);
        }
        close(fd);
        return !expect_success ? 0 : -1;
    }
}

void _start(const char *arg)
{
    if (arg == 0) {
        printf("uso: tcptest <connect|closed|timeout|stress|concurrent> <ip> <porta> [count]\n");
        exit(1);
    }

    size_t l = 0;
    while (arg[l] != '\0' && l < sizeof(arg_copy) - 1) {
        arg_copy[l] = arg[l];
        l++;
    }
    arg_copy[l] = '\0';

    char *p = arg_copy;
    while (*p == ' ') p++;
    char *cmd = p;
    while (*p != '\0' && *p != ' ') p++;
    if (*p != '\0') {
        *p++ = '\0';
    }

    while (*p == ' ') p++;
    char *ip_str = p;
    while (*p != '\0' && *p != ' ') p++;
    if (*p != '\0') {
        *p++ = '\0';
    }

    while (*p == ' ') p++;
    char *port_str = p;
    while (*p != '\0' && *p != ' ') p++;
    if (*p != '\0') {
        *p++ = '\0';
    }

    while (*p == ' ') p++;
    char *extra_str = p;
    while (*p != '\0' && *p != ' ') p++;
    if (*p != '\0') {
        *p++ = '\0';
    }

    if (cmd[0] == '\0') {
        printf("uso: tcptest <connect|closed|timeout|stress|concurrent> <ip> <porta> [count]\n");
        exit(1);
    }

    unsigned int port = 8080;
    if (port_str[0] != '\0') {
        parse_uint(port_str, &port);
    }

    if (strcmp(cmd, "connect") == 0) {
        int res = do_connect_test(ip_str[0] != '\0' ? ip_str : "10.0.2.2", (uint16_t)port, 1);
        exit(res == 0 ? 0 : 1);
    }
    else if (strcmp(cmd, "closed") == 0) {
        int res = do_connect_test(ip_str[0] != '\0' ? ip_str : "10.0.2.2", (uint16_t)port, 0);
        exit(res == 0 ? 0 : 1);
    }
    else if (strcmp(cmd, "timeout") == 0) {
        int res = do_connect_test(ip_str[0] != '\0' ? ip_str : "10.0.2.240", (uint16_t)port, 0);
        exit(res == 0 ? 0 : 1);
    }
    else if (strcmp(cmd, "stress") == 0) {
        unsigned int count = 4;
        if (extra_str[0] != '\0') {
            parse_uint(extra_str, &count);
        }
        printf("[TCPTEST STRESS] Executando %u conexoes consecutivas contra %s:%u...\n",
            count, ip_str[0] != '\0' ? ip_str : "10.0.2.2", port);

        int all_ok = 1;
        for (unsigned int i = 1; i <= count; i++) {
            printf("[TCPTEST STRESS %u/%u] ", i, count);
            if (do_connect_test(ip_str[0] != '\0' ? ip_str : "10.0.2.2", (uint16_t)port, 1) != 0) {
                all_ok = 0;
            }
        }
        if (all_ok) {
            printf("[TCPTEST STRESS] >>> TODAS AS %u CONEXOES CONSECUTIVAS PASSARAM COM SUCESSO! <<<\n", count);
            exit(0);
        } else {
            printf("[TCPTEST STRESS] >>> FALHA EM UMA OU MAIS CONEXOES CONSECUTIVAS! <<<\n");
            exit(1);
        }
    }
    else if (strcmp(cmd, "concurrent") == 0) {
        unsigned int count = 4;
        if (extra_str[0] != '\0') {
            parse_uint(extra_str, &count);
        }
        printf("[TCPTEST CONCURRENT] Executando %u conexoes simultaneas via fork()...\n", count);

        int pids[8];
        if (count > 8) count = 8;

        for (unsigned int i = 0; i < count; i++) {
            int pid = fork();
            if (pid == 0) {
                /* Processo filho */
                int ret = do_connect_test(ip_str[0] != '\0' ? ip_str : "10.0.2.2", (uint16_t)port, 1);
                exit(ret == 0 ? 0 : 1);
            }
            pids[i] = pid;
        }

        int all_ok = 1;
        for (unsigned int i = 0; i < count; i++) {
            int status = 0;
            waitpid(pids[i], &status, 0);
            if (status != 0) {
                all_ok = 0;
            }
        }

        if (all_ok) {
            printf("[TCPTEST CONCURRENT] >>> TODAS AS %u CONEXOES SIMULTANEAS PASSARAM COM SUCESSO! <<<\n", count);
            exit(0);
        } else {
            printf("[TCPTEST CONCURRENT] >>> FALHA EM UMA OU MAIS CONEXOES SIMULTANEAS! <<<\n");
            exit(1);
        }
    }
    else {
        printf("Comando desconhecido: %s. Use connect, closed, timeout, stress, concurrent.\n", cmd);
        exit(1);
    }
}
