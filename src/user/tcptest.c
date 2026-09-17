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

static int do_listen_test(uint16_t port, int backlog)
{
    int listener_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener_fd < 0) {
        printf("[TCPTEST SERVER] FAIL: socket() retornou %d\n", listener_fd);
        return -1;
    }

    struct sockaddr_in bind_addr;
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port = htons(port);
    bind_addr.sin_addr.s_addr = 0; /* INADDR_ANY */
    for (int i = 0; i < 8; i++) bind_addr.sin_zero[i] = 0;

    if (bind(listener_fd, (const struct sockaddr *)&bind_addr, sizeof(bind_addr)) != 0) {
        printf("[TCPTEST SERVER] FAIL: bind() na porta %u falhou\n", (unsigned int)port);
        close(listener_fd);
        return -1;
    }

    if (listen(listener_fd, backlog) != 0) {
        printf("[TCPTEST SERVER] FAIL: listen() falhou\n");
        close(listener_fd);
        return -1;
    }

    printf("[TCPTEST SERVER] Ouvindo na porta %u (backlog %d)... aguardando accept()...\n",
        (unsigned int)port, backlog);

    struct sockaddr_in peer;
    uint32_t peer_len = sizeof(peer);
    int client_fd = accept(listener_fd, (struct sockaddr *)&peer, &peer_len);
    if (client_fd < 0) {
        printf("[TCPTEST SERVER] FAIL: accept() retornou %d\n", client_fd);
        close(listener_fd);
        return -1;
    }

    uint8_t *pip = (uint8_t *)&peer.sin_addr.s_addr;
    uint16_t pport = ntohs(peer.sin_port);
    printf("[TCPTEST SERVER] PASS: Conexao aceita com sucesso! fd=%d peer=%u.%u.%u.%u:%u\n",
        client_fd, (unsigned int)pip[0], (unsigned int)pip[1], (unsigned int)pip[2], (unsigned int)pip[3],
        (unsigned int)pport);

    close(client_fd);
    close(listener_fd);
    printf("[TCPTEST SERVER] >>> SERVIDOR ENCERROU COM SUCESSO! <<<\n");
    return 0;
}

static int do_server_multi_test(uint16_t port, unsigned int count, int backlog)
{
    int listener_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener_fd < 0) {
        printf("[TCPTEST MULTI] FAIL: socket()\n");
        return -1;
    }

    struct sockaddr_in bind_addr;
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port = htons(port);
    bind_addr.sin_addr.s_addr = 0;
    for (int i = 0; i < 8; i++) bind_addr.sin_zero[i] = 0;

    if (bind(listener_fd, (const struct sockaddr *)&bind_addr, sizeof(bind_addr)) != 0 ||
        listen(listener_fd, backlog) != 0) {
        printf("[TCPTEST MULTI] FAIL: bind ou listen\n");
        close(listener_fd);
        return -1;
    }

    printf("[TCPTEST MULTI] Ouvindo na porta %u, aceitando %u conexoes...\n", (unsigned int)port, count);

    int all_ok = 1;
    for (unsigned int i = 1; i <= count; i++) {
        struct sockaddr_in peer;
        uint32_t peer_len = sizeof(peer);
        int client_fd = accept(listener_fd, (struct sockaddr *)&peer, &peer_len);
        if (client_fd < 0) {
            printf("[TCPTEST MULTI %u/%u] FAIL: accept() retornou %d\n", i, count, client_fd);
            all_ok = 0;
            break;
        }
        uint8_t *pip = (uint8_t *)&peer.sin_addr.s_addr;
        uint16_t pport = ntohs(peer.sin_port);
        printf("[TCPTEST MULTI %u/%u] PASS: fd=%d peer=%u.%u.%u.%u:%u\n",
            i, count, client_fd,
            (unsigned int)pip[0], (unsigned int)pip[1], (unsigned int)pip[2], (unsigned int)pip[3],
            (unsigned int)pport);
        close(client_fd);
    }

    close(listener_fd);
    if (all_ok) {
        printf("[TCPTEST MULTI] >>> TODAS AS %u CONEXOES MULTIPLAS ACEITAS COM SUCESSO! <<<\n", count);
        return 0;
    }
    return -1;
}

static int do_server_fork_test(uint16_t port)
{
    int listener_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener_fd < 0) {
        printf("[TCPTEST FORK] FAIL: socket()\n");
        return -1;
    }

    struct sockaddr_in bind_addr;
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port = htons(port);
    bind_addr.sin_addr.s_addr = 0;
    for (int i = 0; i < 8; i++) bind_addr.sin_zero[i] = 0;

    if (bind(listener_fd, (const struct sockaddr *)&bind_addr, sizeof(bind_addr)) != 0 ||
        listen(listener_fd, 4) != 0) {
        printf("[TCPTEST FORK] FAIL: bind/listen\n");
        close(listener_fd);
        return -1;
    }

    printf("[TCPTEST FORK] Aguardando conexao para fork...\n");
    struct sockaddr_in peer;
    uint32_t peer_len = sizeof(peer);
    int client_fd = accept(listener_fd, (struct sockaddr *)&peer, &peer_len);
    if (client_fd < 0) {
        printf("[TCPTEST FORK] FAIL: accept()\n");
        close(listener_fd);
        return -1;
    }

    int pid = fork();
    if (pid == 0) {
        close(listener_fd);
        printf("[TCPTEST FORK CHILD] Filho executando com client_fd=%d\n", client_fd);
        close(client_fd);
        exit(0);
    } else if (pid > 0) {
        int status = 0;
        waitpid(pid, &status, 0);
        close(client_fd);
        close(listener_fd);
        if (status == 0) {
            printf("[TCPTEST FORK] >>> TESTE DE FORK E LIFECYCLE DE DESCRITORES PASSOU COM SUCESSO! <<<\n");
            return 0;
        } else {
            printf("[TCPTEST FORK] FAIL: status do filho = %d\n", status);
            return -1;
        }
    } else {
        printf("[TCPTEST FORK] FAIL: fork() retornou %d\n", pid);
        close(client_fd);
        close(listener_fd);
        return -1;
    }
    return 0;
}

static int do_error_tests(void)
{
    printf("[TCPTEST ERRORS] Iniciando testes de validacao de erros...\n");

    /* 1. listen em fd invalido */
    if (listen(-1, 5) != -1) {
        printf("[TCPTEST ERRORS] FAIL: listen(-1) deveria falhar\n");
        return -1;
    }

    /* 2. listen em socket UDP */
    int udp_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (udp_fd >= 0) {
        if (listen(udp_fd, 5) != -1) {
            printf("[TCPTEST ERRORS] FAIL: listen(UDP) deveria falhar\n");
            close(udp_fd);
            return -1;
        }
        close(udp_fd);
    }

    /* 3. accept em fd invalido */
    if (accept(-1, 0, 0) != -1) {
        printf("[TCPTEST ERRORS] FAIL: accept(-1) deveria falhar\n");
        return -1;
    }

    /* 4. accept em socket nao-LISTEN */
    int tcp_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (tcp_fd >= 0) {
        if (accept(tcp_fd, 0, 0) != -1) {
            printf("[TCPTEST ERRORS] FAIL: accept(CLOSED) deveria falhar\n");
            close(tcp_fd);
            return -1;
        }
        close(tcp_fd);
    }

    /* 5. accept em fd ja fechado */
    if (accept(tcp_fd, 0, 0) != -1) {
        printf("[TCPTEST ERRORS] FAIL: accept(ja fechado) deveria falhar\n");
        return -1;
    }

    printf("[TCPTEST ERRORS] >>> TODOS OS TESTES DE ERRO PASSARAM COM SUCESSO! <<<\n");
    return 0;
}

static int do_recv_server_test(uint16_t port, unsigned int expected_bytes)
{
    int listener_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener_fd < 0) {
        printf("[TCPTEST RECV] FAIL: socket()\n");
        return -1;
    }

    struct sockaddr_in bind_addr;
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port = htons(port);
    bind_addr.sin_addr.s_addr = 0;
    for (int i = 0; i < 8; i++) bind_addr.sin_zero[i] = 0;

    if (bind(listener_fd, (const struct sockaddr *)&bind_addr, sizeof(bind_addr)) != 0 ||
        listen(listener_fd, 4) != 0) {
        printf("[TCPTEST RECV] FAIL: bind/listen\n");
        close(listener_fd);
        return -1;
    }

    printf("[TCPTEST RECV] Ouvindo na porta %u, aguardando conexao...\n", (unsigned int)port);
    struct sockaddr_in peer;
    uint32_t peer_len = sizeof(peer);
    int client_fd = accept(listener_fd, (struct sockaddr *)&peer, &peer_len);
    if (client_fd < 0) {
        printf("[TCPTEST RECV] FAIL: accept()\n");
        close(listener_fd);
        return -1;
    }

    printf("[TCPTEST RECV] Conexao aceita fd=%d. Lendo dados...\n", client_fd);
    char buf[2048];
    unsigned int total_read = 0;
    while (expected_bytes == 0 || total_read < expected_bytes) {
        int r = recv(client_fd, buf + total_read, sizeof(buf) - 1 - total_read, 0);
        if (r < 0) {
            printf("[TCPTEST RECV] ERRO no recv() retornou %d\n", r);
            close(client_fd);
            close(listener_fd);
            return -1;
        }
        if (r == 0) {
            printf("[TCPTEST RECV] EOF recebido do peer (FIN)\n");
            break;
        }
        total_read += (unsigned int)r;
        buf[total_read] = '\0';
        printf("[TCPTEST RECV CHUNK] lidos %d bytes (total=%u): %s\n", r, total_read, buf);
    }

    buf[total_read] = '\0';
    printf("[TCPTEST RECV TOTAL] Recebidos %u bytes com sucesso: %s\n", total_read, buf);
    close(client_fd);
    close(listener_fd);
    return 0;
}

static int do_recv_partial_test(uint16_t port)
{
    int listener_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener_fd < 0) return -1;

    struct sockaddr_in bind_addr;
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port = htons(port);
    bind_addr.sin_addr.s_addr = 0;
    for (int i = 0; i < 8; i++) bind_addr.sin_zero[i] = 0;

    if (bind(listener_fd, (const struct sockaddr *)&bind_addr, sizeof(bind_addr)) != 0 ||
        listen(listener_fd, 4) != 0) {
        close(listener_fd);
        return -1;
    }

    printf("[TCPTEST PARTIAL] Ouvindo na porta %u...\n", (unsigned int)port);
    int client_fd = accept(listener_fd, 0, 0);
    if (client_fd < 0) {
        close(listener_fd);
        return -1;
    }

    char chunk1[8];
    int r1 = recv(client_fd, chunk1, 4, 0);
    if (r1 > 0) chunk1[r1] = '\0';
    printf("[TCPTEST PARTIAL 1] lidos %d bytes: %s\n", r1, chunk1);

    char chunk2[8];
    int r2 = recv(client_fd, chunk2, 4, 0);
    if (r2 > 0) chunk2[r2] = '\0';
    printf("[TCPTEST PARTIAL 2] lidos %d bytes: %s\n", r2, chunk2);

    char chunk3[16];
    int r3 = recv(client_fd, chunk3, sizeof(chunk3) - 1, 0);
    if (r3 > 0) chunk3[r3] = '\0';
    printf("[TCPTEST PARTIAL 3] lidos %d bytes: %s\n", r3, chunk3);

    close(client_fd);
    close(listener_fd);
    if (r1 == 4 && r2 == 4 && r3 >= 2) {
        printf("[TCPTEST PARTIAL] >>> LEITURAS PARCIAIS CONCLUIDAS COM SUCESSO! <<<\n");
        return 0;
    }
    return -1;
}

static int do_recv_block_test(uint16_t port)
{
    int listener_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener_fd < 0) return -1;

    struct sockaddr_in bind_addr;
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port = htons(port);
    bind_addr.sin_addr.s_addr = 0;
    for (int i = 0; i < 8; i++) bind_addr.sin_zero[i] = 0;

    if (bind(listener_fd, (const struct sockaddr *)&bind_addr, sizeof(bind_addr)) != 0 ||
        listen(listener_fd, 4) != 0) {
        close(listener_fd);
        return -1;
    }

    printf("[TCPTEST BLOCK] Aguardando conexao na porta %u...\n", (unsigned int)port);
    int client_fd = accept(listener_fd, 0, 0);
    if (client_fd < 0) {
        close(listener_fd);
        return -1;
    }

    printf("[TCPTEST BLOCK] Conexao estabelecida. Chamando recv() bloqueante...\n");
    char buf[64];
    int r = recv(client_fd, buf, sizeof(buf) - 1, 0);
    if (r > 0) {
        buf[r] = '\0';
        printf("[TCPTEST BLOCK] Acordou do recv() com sucesso! lidos %d bytes: %s\n", r, buf);
        close(client_fd);
        close(listener_fd);
        return 0;
    } else {
        printf("[TCPTEST BLOCK] FAIL: recv() retornou %d\n", r);
        close(client_fd);
        close(listener_fd);
        return -1;
    }
}

static int do_recv_error_test(void)
{
    printf("[TCPTEST RECV ERRORS] Iniciando validacoes de erro de recv()...\n");
    char buf[32];

    /* 1. recv em fd invalido */
    if (recv(-1, buf, 10, 0) != -1) {
        printf("[TCPTEST RECV ERRORS] FAIL: recv(-1) deveria falhar\n");
        return -1;
    }

    int tcp_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (tcp_fd < 0) {
        printf("[TCPTEST RECV ERRORS] FAIL: socket()\n");
        return -1;
    }

    /* 2. recv com buffer NULL */
    if (recv(tcp_fd, 0, 10, 0) != -1) {
        printf("[TCPTEST RECV ERRORS] FAIL: recv(NULL) deveria falhar\n");
        close(tcp_fd);
        return -1;
    }

    /* 3. recv com endereco invalido de memoria do usuario */
    if (recv(tcp_fd, (void *)0x10, 10, 0) != -1) {
        printf("[TCPTEST RECV ERRORS] FAIL: recv(0x10) deveria falhar\n");
        close(tcp_fd);
        return -1;
    }

    /* 4. recv com len = 0 */
    if (recv(tcp_fd, buf, 0, 0) != 0) {
        printf("[TCPTEST RECV ERRORS] FAIL: recv(len=0) deveria retornar 0\n");
        close(tcp_fd);
        return -1;
    }

    /* 5. recv em socket UDP */
    int udp_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (udp_fd >= 0) {
        if (recv(udp_fd, buf, 10, 0) != -1) {
            printf("[TCPTEST RECV ERRORS] FAIL: recv(UDP) deveria falhar\n");
            close(udp_fd);
            close(tcp_fd);
            return -1;
        }
        close(udp_fd);
    }

    /* 6. recv apos close */
    close(tcp_fd);
    if (recv(tcp_fd, buf, 10, 0) != -1) {
        printf("[TCPTEST RECV ERRORS] FAIL: recv(fechado) deveria falhar\n");
        return -1;
    }

    printf("[TCPTEST RECV ERRORS] >>> TODOS OS TESTES DE ERRO RECV PASSARAM! <<<\n");
    return 0;
}

static int do_recv_multi_test(uint16_t port, unsigned int count)
{
    int listener_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener_fd < 0) return -1;

    struct sockaddr_in bind_addr;
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port = htons(port);
    bind_addr.sin_addr.s_addr = 0;
    for (int i = 0; i < 8; i++) bind_addr.sin_zero[i] = 0;

    if (bind(listener_fd, (const struct sockaddr *)&bind_addr, sizeof(bind_addr)) != 0 ||
        listen(listener_fd, 4) != 0) {
        close(listener_fd);
        return -1;
    }

    printf("[TCPTEST RECV MULTI] Ouvindo na porta %u para %u conexoes...\n", (unsigned int)port, count);
    for (unsigned int i = 1; i <= count; i++) {
        int client_fd = accept(listener_fd, 0, 0);
        if (client_fd < 0) {
            printf("[TCPTEST RECV MULTI %u] FAIL: accept()\n", i);
            close(listener_fd);
            return -1;
        }
        char buf[64];
        int r = recv(client_fd, buf, sizeof(buf) - 1, 0);
        if (r > 0) buf[r] = '\0';
        printf("[TCPTEST RECV MULTI %u/%u] lidos %d bytes: %s\n", i, count, r, buf);
        close(client_fd);
    }

    close(listener_fd);
    printf("[TCPTEST RECV MULTI] >>> TODAS AS CONEXOES MULTIPLAS RECEBERAM DADOS COM SUCESSO! <<<\n");
    return 0;
}

static int do_recv_fork_test(uint16_t port)
{
    int listener_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener_fd < 0) return -1;

    struct sockaddr_in bind_addr;
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port = htons(port);
    bind_addr.sin_addr.s_addr = 0;
    for (int i = 0; i < 8; i++) bind_addr.sin_zero[i] = 0;

    if (bind(listener_fd, (const struct sockaddr *)&bind_addr, sizeof(bind_addr)) != 0 ||
        listen(listener_fd, 4) != 0) {
        close(listener_fd);
        return -1;
    }

    printf("[TCPTEST RECV FORK] Aguardando conexao na porta %u...\n", (unsigned int)port);
    int client_fd = accept(listener_fd, 0, 0);
    if (client_fd < 0) {
        close(listener_fd);
        return -1;
    }

    int pid = fork();
    if (pid == 0) {
        close(listener_fd);
        char buf[64];
        int r = recv(client_fd, buf, sizeof(buf) - 1, 0);
        if (r > 0) buf[r] = '\0';
        printf("[TCPTEST RECV FORK CHILD] lidos %d bytes: %s\n", r, buf);
        close(client_fd);
        exit(r > 0 ? 0 : 1);
    } else if (pid > 0) {
        int status = 0;
        waitpid(pid, &status, 0);
        close(client_fd);
        close(listener_fd);
        if (status == 0) {
            printf("[TCPTEST RECV FORK] >>> RECV NO FILHO PASSOU COM SUCESSO! <<<\n");
            return 0;
        } else {
            printf("[TCPTEST RECV FORK] FAIL: status do filho = %d\n", status);
            return -1;
        }
    } else {
        close(client_fd);
        close(listener_fd);
        return -1;
    }
    return 0;
}

static int do_recv_dup_test(uint16_t port)
{
    int listener_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener_fd < 0) return -1;

    struct sockaddr_in bind_addr;
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port = htons(port);
    bind_addr.sin_addr.s_addr = 0;
    for (int i = 0; i < 8; i++) bind_addr.sin_zero[i] = 0;

    if (bind(listener_fd, (const struct sockaddr *)&bind_addr, sizeof(bind_addr)) != 0 ||
        listen(listener_fd, 4) != 0) {
        close(listener_fd);
        return -1;
    }

    printf("[TCPTEST RECV DUP] Aguardando conexao na porta %u...\n", (unsigned int)port);
    int client_fd = accept(listener_fd, 0, 0);
    if (client_fd < 0) {
        close(listener_fd);
        return -1;
    }

    int dup_fd = dup(client_fd);
    if (dup_fd < 0) {
        printf("[TCPTEST RECV DUP] FAIL: dup()\n");
        close(client_fd);
        close(listener_fd);
        return -1;
    }

    char buf[64];
    int r = recv(dup_fd, buf, sizeof(buf) - 1, 0);
    if (r > 0) buf[r] = '\0';
    printf("[TCPTEST RECV DUP] lidos %d bytes via dup_fd=%d: %s\n", r, dup_fd, buf);
    close(dup_fd);
    close(client_fd);
    close(listener_fd);
    if (r > 0) {
        printf("[TCPTEST RECV DUP] >>> RECV VIA DUP PASSOU COM SUCESSO! <<<\n");
        return 0;
    }
    return -1;
}

static void delay_ticks(uint64_t ticks)
{
    uint64_t start = get_ticks();
    while (get_ticks() - start < ticks) {
        yield();
    }
}

static int do_send_server_test(uint16_t port, const char *mode)
{
    int listener_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener_fd < 0) {
        printf("[TCPTEST SEND] FAIL: socket()\n");
        return -1;
    }

    struct sockaddr_in bind_addr;
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port = htons(port);
    bind_addr.sin_addr.s_addr = 0;
    for (int i = 0; i < 8; i++) bind_addr.sin_zero[i] = 0;

    if (bind(listener_fd, (const struct sockaddr *)&bind_addr, sizeof(bind_addr)) != 0 ||
        listen(listener_fd, 4) != 0) {
        printf("[TCPTEST SEND] FAIL: bind/listen\n");
        close(listener_fd);
        return -1;
    }

    printf("[TCPTEST SEND] Ouvindo na porta %u (modo %s)... aguardando conexao...\n",
        (unsigned int)port, mode != 0 ? mode : "default");

    int client_fd = accept(listener_fd, 0, 0);
    if (client_fd < 0) {
        printf("[TCPTEST SEND] FAIL: accept()\n");
        close(listener_fd);
        return -1;
    }

    int ret = 0;
    if (strcmp(mode, "single") == 0) {
        /* Envia 1 byte 'A' */
        int s = send(client_fd, "A", 1, 0);
        printf("[TCPTEST SEND SINGLE] send() retornou %d\n", s);
        if (s != 1) ret = -1;
    } else if (strcmp(mode, "small") == 0) {
        /* Envia "Hello PhotonOS" (14 bytes) */
        const char *msg = "Hello PhotonOS";
        int s = send(client_fd, msg, 14, 0);
        printf("[TCPTEST SEND SMALL] send() retornou %d\n", s);
        if (s != 14) ret = -1;
    } else if (strcmp(mode, "1024") == 0) {
        /* Envia 1024 bytes */
        static uint8_t pat[1024];
        for (int i = 0; i < 1024; i++) {
            pat[i] = (uint8_t)('A' + (i % 26));
        }
        int s = send(client_fd, pat, 1024, 0);
        printf("[TCPTEST SEND 1024] send() retornou %d\n", s);
        if (s != 1024) ret = -1;
    } else if (strcmp(mode, "seg") == 0) {
        /* Envia 2000 bytes (testa segmentacao MSS 1460 + 540) */
        static uint8_t pat2[2000];
        for (int i = 0; i < 2000; i++) {
            pat2[i] = (uint8_t)('0' + (i % 10));
        }
        int s = send(client_fd, pat2, 2000, 0);
        printf("[TCPTEST SEND SEG] send() retornou %d\n", s);
        if (s != 2000) ret = -1;
    } else if (strcmp(mode, "large") == 0) {
        /* Envia 8192 bytes */
        static uint8_t pat3[8192];
        for (int i = 0; i < 8192; i++) {
            pat3[i] = (uint8_t)('a' + (i % 26));
        }
        size_t total = 0;
        while (total < 8192) {
            int n = send(client_fd, pat3 + total, 8192 - total, 0);
            if (n <= 0) break;
            total += (size_t)n;
        }
        printf("[TCPTEST SEND LARGE] total enviado %u bytes\n", (unsigned int)total);
        if (total != 8192) ret = -1;
    } else if (strcmp(mode, "large16") == 0) {
        /*
         * Deliberately exceeds the 8192-byte TX buffer.  send() may return a
         * short write, so retry after yielding to let peer ACKs free space.
         */
        static uint8_t pat4[16384];
        for (int i = 0; i < 16384; i++) {
            pat4[i] = (uint8_t)('A' + (i % 26));
        }
        size_t total = 0;
        int stalled = 0;
        while (total < 16384 && stalled < 64) {
            int n = send(client_fd, pat4 + total, 16384 - total, 0);
            if (n > 0) {
                total += (size_t)n;
                stalled = 0;
            } else {
                stalled++;
                yield();
            }
        }
        printf("[TCPTEST SEND LARGE16] total enviado %u bytes\n", (unsigned int)total);
        if (total != 16384) ret = -1;
    }

    delay_ticks(20);
    close(client_fd);
    close(listener_fd);

    if (ret == 0) {
        printf("[TCPTEST SEND] >>> SERVIDOR SEND ENCERROU COM SUCESSO! <<<\n");
    } else {
        printf("[TCPTEST SEND] >>> FALHA NO SERVIDOR SEND! <<<\n");
    }
    return ret;
}

static int do_send_multi_test(uint16_t port, unsigned int count)
{
    int listener_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener_fd < 0) {
        printf("[TCPTEST MULTI SEND] FAIL: socket()\n");
        return -1;
    }

    struct sockaddr_in bind_addr;
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port = htons(port);
    bind_addr.sin_addr.s_addr = 0;
    for (int i = 0; i < 8; i++) bind_addr.sin_zero[i] = 0;

    if (bind(listener_fd, (const struct sockaddr *)&bind_addr, sizeof(bind_addr)) != 0 ||
        listen(listener_fd, (int)count + 1) != 0) {
        printf("[TCPTEST MULTI SEND] FAIL: bind/listen\n");
        close(listener_fd);
        return -1;
    }

    printf("[TCPTEST MULTI SEND] Ouvindo na porta %u para %u conexoes...\n", (unsigned int)port, count);

    int all_ok = 1;
    for (unsigned int i = 0; i < count; i++) {
        int client_fd = accept(listener_fd, 0, 0);
        if (client_fd < 0) {
            printf("[TCPTEST MULTI SEND] FAIL: accept() conexao %u\n", i);
            all_ok = 0;
            break;
        }
        char msg[32];
        msg[0] = 'M'; msg[1] = 'S'; msg[2] = 'G'; msg[3] = '_';
        msg[4] = (char)('0' + (i % 10));
        msg[5] = '\0';
        int s = send(client_fd, msg, 5, 0);
        printf("[TCPTEST MULTI SEND] Conexao %u enviou %d bytes: %s\n", i, s, msg);
        if (s != 5) all_ok = 0;
        delay_ticks(20);
        close(client_fd);
    }

    close(listener_fd);
    if (all_ok) {
        printf("[TCPTEST MULTI SEND] >>> MULTI SEND PASSOU COM SUCESSO! <<<\n");
        return 0;
    }
    return -1;
}

static int do_send_fork_test(uint16_t port)
{
    int listener_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener_fd < 0) {
        printf("[TCPTEST FORK SEND] FAIL: socket()\n");
        return -1;
    }

    struct sockaddr_in bind_addr;
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port = htons(port);
    bind_addr.sin_addr.s_addr = 0;
    for (int i = 0; i < 8; i++) bind_addr.sin_zero[i] = 0;

    if (bind(listener_fd, (const struct sockaddr *)&bind_addr, sizeof(bind_addr)) != 0 ||
        listen(listener_fd, 2) != 0) {
        printf("[TCPTEST FORK SEND] FAIL: bind/listen\n");
        close(listener_fd);
        return -1;
    }

    printf("[TCPTEST FORK SEND] Aguardando conexao na porta %u...\n", (unsigned int)port);
    int client_fd = accept(listener_fd, 0, 0);
    if (client_fd < 0) {
        close(listener_fd);
        return -1;
    }

    int pid = fork();
    if (pid == 0) {
        /* Processo filho envia dados */
        int s = send(client_fd, "FORK_DATA_OK", 12, 0);
        printf("[TCPTEST FORK SEND] Filho enviou %d bytes\n", s);
        delay_ticks(20);
        close(client_fd);
        close(listener_fd);
        exit(s == 12 ? 0 : 1);
    }

    /* Processo pai fecha client_fd e aguarda filho */
    close(client_fd);
    int status = 0;
    waitpid(pid, &status, 0);
    close(listener_fd);

    if (status == 0) {
        printf("[TCPTEST FORK SEND] >>> FORK SEND PASSOU COM SUCESSO! <<<\n");
        return 0;
    }
    return -1;
}

static int do_send_dup_test(uint16_t port)
{
    int listener_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener_fd < 0) {
        printf("[TCPTEST DUP SEND] FAIL: socket()\n");
        return -1;
    }

    struct sockaddr_in bind_addr;
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port = htons(port);
    bind_addr.sin_addr.s_addr = 0;
    for (int i = 0; i < 8; i++) bind_addr.sin_zero[i] = 0;

    if (bind(listener_fd, (const struct sockaddr *)&bind_addr, sizeof(bind_addr)) != 0 ||
        listen(listener_fd, 2) != 0) {
        printf("[TCPTEST DUP SEND] FAIL: bind/listen\n");
        close(listener_fd);
        return -1;
    }

    printf("[TCPTEST DUP SEND] Aguardando conexao na porta %u...\n", (unsigned int)port);
    int client_fd = accept(listener_fd, 0, 0);
    if (client_fd < 0) {
        close(listener_fd);
        return -1;
    }

    int dup_fd = dup(client_fd);
    if (dup_fd < 0) {
        printf("[TCPTEST DUP SEND] FAIL: dup()\n");
        close(client_fd);
        close(listener_fd);
        return -1;
    }

    int s = send(dup_fd, "DUP_DATA_OK", 11, 0);
    printf("[TCPTEST DUP SEND] enviado via dup_fd=%d: res=%d\n", dup_fd, s);
    delay_ticks(20);
    close(dup_fd);
    close(client_fd);
    close(listener_fd);

    if (s == 11) {
        printf("[TCPTEST DUP SEND] >>> DUP SEND PASSOU COM SUCESSO! <<<\n");
        return 0;
    }
    return -1;
}

static int do_send_error_test(void)
{
    printf("[TCPTEST SEND ERRORS] Iniciando testes negativos de send()...\n");
    int all_ok = 1;

    /* 1. Bad file descriptor */
    int r1 = send(-1, "A", 1, 0);
    if (r1 < 0) {
        printf("[TCPTEST SEND ERRORS] PASS: send(fd=-1) retornou %d\n", r1);
    } else {
        printf("[TCPTEST SEND ERRORS] FAIL: send(fd=-1) retornou %d\n", r1);
        all_ok = 0;
    }

    /* 2. Socket closed / not established */
    int s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    int r2 = send(s, "A", 1, 0);
    if (r2 < 0) {
        printf("[TCPTEST SEND ERRORS] PASS: send(sock CLOSED) retornou %d\n", r2);
    } else {
        printf("[TCPTEST SEND ERRORS] FAIL: send(sock CLOSED) retornou %d\n", r2);
        all_ok = 0;
    }

    /* 3. NULL pointer */
    int r3 = send(s, 0, 10, 0);
    if (r3 < 0) {
        printf("[TCPTEST SEND ERRORS] PASS: send(NULL) retornou %d\n", r3);
    } else {
        printf("[TCPTEST SEND ERRORS] FAIL: send(NULL) retornou %d\n", r3);
        all_ok = 0;
    }

    /* 4. Invalid pointer */
    int r4 = send(s, (const void *)0x10, 10, 0);
    if (r4 < 0) {
        printf("[TCPTEST SEND ERRORS] PASS: send(0x10) retornou %d\n", r4);
    } else {
        printf("[TCPTEST SEND ERRORS] FAIL: send(0x10) retornou %d\n", r4);
        all_ok = 0;
    }

    /*
     * 5. Range whose first byte is in the user-half but whose second byte
     * crosses VMM_USER_LIMIT.  The syscall must reject the complete range
     * before it considers the closed socket or copies any payload.
     */
    int r5 = send(s, (const void *)0x00007FFFFFFFFFFFULL, 2, 0);
    if (r5 < 0) {
        printf("[TCPTEST SEND ERRORS] PASS: send(partial invalid range) retornou %d\n", r5);
    } else {
        printf("[TCPTEST SEND ERRORS] FAIL: send(partial invalid range) retornou %d\n", r5);
        all_ok = 0;
    }

    /* 6. Zero length */
    int r6 = send(s, "A", 0, 0);
    if (r6 == 0) {
        printf("[TCPTEST SEND ERRORS] PASS: send(len=0) retornou 0\n");
    } else {
        printf("[TCPTEST SEND ERRORS] FAIL: send(len=0) retornou %d\n", r6);
        all_ok = 0;
    }

    /* 7. Send on listening socket */
    struct sockaddr_in bind_addr;
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port = htons(9876);
    bind_addr.sin_addr.s_addr = 0;
    for (int i = 0; i < 8; i++) bind_addr.sin_zero[i] = 0;
    bind(s, (const struct sockaddr *)&bind_addr, sizeof(bind_addr));
    listen(s, 1);
    int r7 = send(s, "A", 1, 0);
    if (r7 < 0) {
        printf("[TCPTEST SEND ERRORS] PASS: send(listener) retornou %d\n", r7);
    } else {
        printf("[TCPTEST SEND ERRORS] FAIL: send(listener) retornou %d\n", r7);
        all_ok = 0;
    }

    /* 8. A datagram socket is not a TCP transmit path. */
    int udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    int r8 = udp >= 0 ? send(udp, "A", 1, 0) : -1;
    if (r8 < 0) {
        printf("[TCPTEST SEND ERRORS] PASS: send(socket nao TCP) retornou %d\n", r8);
    } else {
        printf("[TCPTEST SEND ERRORS] FAIL: send(socket nao TCP) retornou %d\n", r8);
        all_ok = 0;
    }
    if (udp >= 0) close(udp);

    close(s);

    if (all_ok) {
        printf("[TCPTEST SEND ERRORS] >>> TODOS OS TESTES NEGATIVOS DE SEND PASSARAM COM SUCESSO! <<<\n");
        return 0;
    }
    return -1;
}

void _start(const char *arg)
{
    if (arg == 0) {
        printf("uso: tcptest <connect|closed|timeout|stress|concurrent|listen|server_multi|server_fork|errors|send_server> ...\n");
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
        printf("uso: tcptest <connect|closed|timeout|stress|concurrent|listen|server_multi|server_fork|errors|send_server> ...\n");
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
    else if (strcmp(cmd, "listen") == 0) {
        /* uso: tcptest listen <porta> [backlog] */
        unsigned int lport = 8088;
        if (ip_str[0] != '\0') {
            parse_uint(ip_str, &lport);
        }
        unsigned int bl = 4;
        if (port_str[0] != '\0') {
            parse_uint(port_str, &bl);
        }
        int res = do_listen_test((uint16_t)lport, (int)bl);
        exit(res == 0 ? 0 : 1);
    }
    else if (strcmp(cmd, "server_multi") == 0) {
        /* uso: tcptest server_multi <porta> <count> [backlog] */
        unsigned int lport = 8088;
        if (ip_str[0] != '\0') {
            parse_uint(ip_str, &lport);
        }
        unsigned int count = 3;
        if (port_str[0] != '\0') {
            parse_uint(port_str, &count);
        }
        unsigned int bl = 4;
        if (extra_str[0] != '\0') {
            parse_uint(extra_str, &bl);
        }
        int res = do_server_multi_test((uint16_t)lport, count, (int)bl);
        exit(res == 0 ? 0 : 1);
    }
    else if (strcmp(cmd, "server_fork") == 0) {
        /* uso: tcptest server_fork <porta> */
        unsigned int lport = 8088;
        if (ip_str[0] != '\0') {
            parse_uint(ip_str, &lport);
        }
        int res = do_server_fork_test((uint16_t)lport);
        exit(res == 0 ? 0 : 1);
    }
    else if (strcmp(cmd, "errors") == 0) {
        int res = do_error_tests();
        exit(res == 0 ? 0 : 1);
    }
    else if (strcmp(cmd, "recv_server") == 0) {
        /* uso: tcptest recv_server <porta> [expected_bytes] */
        unsigned int lport = 8088;
        if (ip_str[0] != '\0') {
            parse_uint(ip_str, &lport);
        }
        unsigned int exp_b = 0;
        if (port_str[0] != '\0') {
            parse_uint(port_str, &exp_b);
        }
        int res = do_recv_server_test((uint16_t)lport, exp_b);
        exit(res == 0 ? 0 : 1);
    }
    else if (strcmp(cmd, "recv_partial") == 0) {
        /* uso: tcptest recv_partial <porta> */
        unsigned int lport = 8088;
        if (ip_str[0] != '\0') {
            parse_uint(ip_str, &lport);
        }
        int res = do_recv_partial_test((uint16_t)lport);
        exit(res == 0 ? 0 : 1);
    }
    else if (strcmp(cmd, "recv_block") == 0) {
        /* uso: tcptest recv_block <porta> */
        unsigned int lport = 8088;
        if (ip_str[0] != '\0') {
            parse_uint(ip_str, &lport);
        }
        int res = do_recv_block_test((uint16_t)lport);
        exit(res == 0 ? 0 : 1);
    }
    else if (strcmp(cmd, "recv_errors") == 0) {
        /* uso: tcptest recv_errors */
        int res = do_recv_error_test();
        exit(res == 0 ? 0 : 1);
    }
    else if (strcmp(cmd, "recv_multi") == 0) {
        /* uso: tcptest recv_multi <porta> <count> */
        unsigned int lport = 8088;
        if (ip_str[0] != '\0') {
            parse_uint(ip_str, &lport);
        }
        unsigned int count = 3;
        if (port_str[0] != '\0') {
            parse_uint(port_str, &count);
        }
        int res = do_recv_multi_test((uint16_t)lport, count);
        exit(res == 0 ? 0 : 1);
    }
    else if (strcmp(cmd, "recv_fork") == 0) {
        /* uso: tcptest recv_fork <porta> */
        unsigned int lport = 8088;
        if (ip_str[0] != '\0') {
            parse_uint(ip_str, &lport);
        }
        int res = do_recv_fork_test((uint16_t)lport);
        exit(res == 0 ? 0 : 1);
    }
    else if (strcmp(cmd, "recv_dup") == 0) {
        /* uso: tcptest recv_dup <porta> */
        unsigned int lport = 8088;
        if (ip_str[0] != '\0') {
            parse_uint(ip_str, &lport);
        }
        int res = do_recv_dup_test((uint16_t)lport);
        exit(res == 0 ? 0 : 1);
    }
    else if (strcmp(cmd, "send_server") == 0) {
        /* uso: tcptest send_server <porta> [mode] */
        unsigned int lport = 8088;
        if (ip_str[0] != '\0') {
            parse_uint(ip_str, &lport);
        }
        const char *mode = "single";
        if (port_str[0] != '\0') {
            mode = port_str;
        }
        int res = do_send_server_test((uint16_t)lport, mode);
        exit(res == 0 ? 0 : 1);
    }
    else if (strcmp(cmd, "send_multi") == 0) {
        /* uso: tcptest send_multi <porta> <count> */
        unsigned int lport = 8088;
        if (ip_str[0] != '\0') {
            parse_uint(ip_str, &lport);
        }
        unsigned int count = 3;
        if (port_str[0] != '\0') {
            parse_uint(port_str, &count);
        }
        int res = do_send_multi_test((uint16_t)lport, count);
        exit(res == 0 ? 0 : 1);
    }
    else if (strcmp(cmd, "send_fork") == 0) {
        /* uso: tcptest send_fork <porta> */
        unsigned int lport = 8088;
        if (ip_str[0] != '\0') {
            parse_uint(ip_str, &lport);
        }
        int res = do_send_fork_test((uint16_t)lport);
        exit(res == 0 ? 0 : 1);
    }
    else if (strcmp(cmd, "send_dup") == 0) {
        /* uso: tcptest send_dup <porta> */
        unsigned int lport = 8088;
        if (ip_str[0] != '\0') {
            parse_uint(ip_str, &lport);
        }
        int res = do_send_dup_test((uint16_t)lport);
        exit(res == 0 ? 0 : 1);
    }
    else if (strcmp(cmd, "send_errors") == 0) {
        /* uso: tcptest send_errors */
        int res = do_send_error_test();
        exit(res == 0 ? 0 : 1);
    }
    else {
        printf("Comando desconhecido: %s. Use connect, closed, timeout, stress, concurrent, listen, server_multi, server_fork, errors, recv_server, recv_partial, recv_block, recv_errors, recv_multi, recv_fork, recv_dup, send_server, send_multi, send_fork, send_dup, send_errors.\n", cmd);
        exit(1);
    }
}
