#include <stddef.h>
#include <stdint.h>

#include "proc.h"

#define SYS_WRITE 1
#define SYS_OPEN 2
#define SYS_READ 3
#define SYS_SPAWN 4
#define SYS_CREATE 6
#define SYS_WAIT 7
#define SYS_PIPE 8
#define SYS_BRK 9
#define SYS_SIGNAL 10
#define SYS_KILL 11
#define SYS_SIGRETURN 12
#define SYS_GETPROCS 13
#define SYS_DUP2 14
#define SYS_CLOSE 15
#define SYS_LIST 16
#define SIGINT 2

#define FD_STDIN_SAVE 29
#define FD_STDOUT_SAVE 30

void *malloc(size_t size);
void free(void *ptr);

static unsigned int next_background_job = 1;
static volatile int shell_interrupted;

static long syscall1(long number, long arg1)
{
    long ret;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"(number), "D"(arg1)
        : "rcx", "r11", "memory");
    return ret;
}

static long syscall2(long number, long arg1, long arg2)
{
    long ret;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"(number), "D"(arg1), "S"(arg2)
        : "rcx", "r11", "memory");
    return ret;
}

static long syscall3(long number, long arg1, long arg2, long arg3)
{
    long ret;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"(number), "D"(arg1), "S"(arg2), "d"(arg3)
        : "rcx", "r11", "memory");
    return ret;
}

static size_t strlen(const char *str)
{
    size_t len = 0;

    while (str[len] != '\0') {
        len++;
    }

    return len;
}

static void write_buf(const char *str, size_t len)
{
    syscall3(SYS_WRITE, 1, (long)str, (long)len);
}

static void write_str(const char *str)
{
    write_buf(str, strlen(str));
}

static int streq(const char *left, const char *right)
{
    size_t i = 0;

    while (left[i] != '\0' && right[i] != '\0') {
        if (left[i] != right[i]) {
            return 0;
        }
        i++;
    }

    return left[i] == right[i];
}

static int starts_with(const char *str, const char *prefix)
{
    size_t i = 0;

    while (prefix[i] != '\0') {
        if (str[i] != prefix[i]) {
            return 0;
        }
        i++;
    }

    return 1;
}

static char *skip_spaces(char *str)
{
    while (*str == ' ') {
        str++;
    }

    return str;
}

static char *find_redirect(char *str)
{
    while (*str != '\0') {
        if (*str == '>') {
            return str;
        }
        str++;
    }

    return 0;
}

static char *find_pipe(char *str)
{
    while (*str != '\0') {
        if (*str == '|') {
            return str;
        }
        str++;
    }

    return 0;
}

static void trim_right(char *str)
{
    size_t len = strlen(str);

    while (len > 0 && str[len - 1] == ' ') {
        str[len - 1] = '\0';
        len--;
    }
}

static int detach_background_marker(char *str)
{
    size_t len;

    trim_right(str);
    len = strlen(str);
    if (len == 0 || str[len - 1] != '&') {
        return 0;
    }

    str[len - 1] = '\0';
    trim_right(str);
    return 1;
}

static void read_line(char *buffer, size_t capacity)
{
    size_t length = 0;

    for (;;) {
        char ch;
        long read = syscall3(SYS_READ, 0, (long)&ch, 1);

        if (read <= 0) {
            if (shell_interrupted) {
                shell_interrupted = 0;
                buffer[0] = '\0';
                return;
            }
            __asm__ volatile ("pause");
            continue;
        }

        if (ch == '\n' || ch == '\r') {
            write_str("\n");
            buffer[length] = '\0';
            return;
        }

        if (ch == '\b') {
            if (length > 0) {
                buffer[--length] = '\0';
                write_str("\b \b");
            }
            continue;
        }

        if (length < capacity - 1) {
            buffer[length++] = ch;
            write_buf(&ch, 1);
        }
    }
}

static void command_ls(void)
{
    char buffer[256];
    long bytes = syscall3(SYS_LIST, (long)"/", (long)buffer, sizeof(buffer));

    if (bytes > 0) {
        write_buf(buffer, (size_t)bytes);
    }
}

static const char *process_state_name(uint32_t state)
{
    if (state == PROC_STATE_READY) {
        return "READY";
    }
    if (state == PROC_STATE_RUNNING) {
        return "RUNNING";
    }
    if (state == PROC_STATE_SLEEPING) {
        return "SLEEPING";
    }
    if (state == PROC_STATE_WAITING) {
        return "WAITING";
    }
    if (state == PROC_STATE_BLOCKED) {
        return "BLOCKED";
    }
    if (state == PROC_STATE_ZOMBIE) {
        return "ZOMBIE";
    }

    return "UNKNOWN";
}

static void write_column(const char *text, size_t width)
{
    size_t len = strlen(text);

    write_buf(text, len);
    while (len < width) {
        write_buf(" ", 1);
        len++;
    }
}

static void command_ps(void)
{
    const size_t capacity = 16;
    proc_info_t *procs = malloc(sizeof(proc_info_t) * capacity);

    if (procs == 0) {
        write_str("ps: falha ao alocar heap\n");
        return;
    }

    long count = syscall2(SYS_GETPROCS, (long)procs,
        (long)(sizeof(proc_info_t) * capacity));
    if (count < 0) {
        free(procs);
        write_str("ps: syscall getprocs falhou\n");
        return;
    }

    write_str("PID      NOME          ESTADO\n");
    for (long i = 0; i < count; i++) {
        char pidbuf[11];
        size_t pidlen = 0;
        uint32_t pid = procs[i].pid;

        if (pid == 0) {
            pidbuf[pidlen++] = '0';
        } else {
            char tmp[10];
            size_t tmplen = 0;
            while (pid > 0 && tmplen < sizeof(tmp)) {
                tmp[tmplen++] = (char)('0' + (pid % 10U));
                pid /= 10U;
            }
            while (tmplen > 0) {
                pidbuf[pidlen++] = tmp[--tmplen];
            }
        }
        pidbuf[pidlen] = '\0';

        write_column(pidbuf, 9);
        write_column(procs[i].name, 14);
        write_str(process_state_name(procs[i].state));
        write_str((procs[i].flags & PROC_FLAG_FOREGROUND) ? " (FG)" : " (BG)");
        write_str("\n");
    }

    free(procs);
}

static void command_cat(const char *path)
{
    char buffer[128];
    long fd = syscall1(SYS_OPEN, (long)path);

    if (fd < 0) {
        write_str("cat: arquivo nao encontrado\n");
        return;
    }

    for (;;) {
        long bytes = syscall3(SYS_READ, fd, (long)buffer, sizeof(buffer));
        if (bytes <= 0) {
            break;
        }
        write_buf(buffer, (size_t)bytes);
    }

    write_str("\n");
}

static void command_cat_raw(char *path)
{
    char buffer[128];
    long fd;

    path = skip_spaces(path);
    trim_right(path);
    fd = syscall1(SYS_OPEN, (long)path);

    if (fd < 0) {
        write_str("cat: arquivo nao encontrado\n");
        return;
    }

    for (;;) {
        long bytes = syscall3(SYS_READ, fd, (long)buffer, sizeof(buffer));
        if (bytes <= 0) {
            break;
        }
        write_buf(buffer, (size_t)bytes);
    }
}

static void command_echo_raw(char *text)
{
    text = skip_spaces(text);
    trim_right(text);
    write_buf(text, strlen(text));
}

static void command_touch(char *path)
{
    path = skip_spaces(path);
    trim_right(path);

    if (path[0] == '\0') {
        write_str("touch: nome ausente\n");
        return;
    }

    if (syscall1(SYS_CREATE, (long)path) < 0) {
        write_str("touch: falha ao criar arquivo\n");
    }
}

static void command_echo_redirect(char *line)
{
    char *text = line + 5;
    char *redirect = find_redirect(text);
    char *path;
    long fd;

    if (redirect == 0) {
        write_str(text);
        write_str("\n");
        return;
    }

    *redirect = '\0';
    trim_right(text);
    path = skip_spaces(redirect + 1);
    trim_right(path);

    if (path[0] == '\0') {
        write_str("echo: nome ausente\n");
        return;
    }

    fd = syscall1(SYS_CREATE, (long)path);
    if (fd < 0) {
        write_str("echo: falha ao criar arquivo\n");
        return;
    }

    syscall3(SYS_WRITE, fd, (long)text, (long)strlen(text));
}

static void wait_pid(long pid)
{
    long state;

    do {
        state = syscall1(SYS_WAIT, pid);
        if (state > 0) {
            __asm__ volatile ("pause");
        }
    } while (state > 0);
}

static long spawn_program(const char *path)
{
    long pid = syscall1(SYS_SPAWN, (long)path);

    if (pid < 0) {
        write_str("spawn: falha ao executar programa\n");
    }

    return pid;
}

static size_t append_text(char *buffer, size_t offset, const char *text)
{
    while (*text != '\0') {
        buffer[offset++] = *text++;
    }

    return offset;
}

static size_t append_uint(char *buffer, size_t offset, unsigned int value)
{
    char digits[10];
    size_t length = 0;

    if (value == 0) {
        buffer[offset++] = '0';
        return offset;
    }

    while (value > 0 && length < sizeof(digits)) {
        digits[length++] = (char)('0' + (value % 10U));
        value /= 10U;
    }

    while (length > 0) {
        buffer[offset++] = digits[--length];
    }

    return offset;
}

static void announce_background(long pid)
{
    char message[80];
    size_t length = 0;

    length = append_text(message, length, "[");
    length = append_uint(message, length, next_background_job++);
    length = append_text(message, length, "] Lancado em background com PID ");
    length = append_uint(message, length, (unsigned int)pid);
    message[length++] = '\n';
    write_buf(message, length);
}

static void command_program(const char *path, int is_background)
{
    long pid = spawn_program(path);

    if (pid < 0) {
        return;
    }

    if (is_background) {
        announce_background(pid);
    } else {
        wait_pid(pid);
    }
}

static void execute_simple(char *line, int is_background)
{
    line = skip_spaces(line);
    trim_right(line);

    if (line[0] == '\0') {
        return;
    }

    if (streq(line, "help")) {
        write_str("help ls ps cat <arquivo> touch <arquivo> echo <texto> > <arquivo> hello upper rev hang spin\n");
    } else if (streq(line, "ls")) {
        command_ls();
    } else if (streq(line, "ps") || streq(line, "jobs")) {
        command_ps();
    } else if (streq(line, "hello")) {
        command_program("/bin/hello", is_background);
    } else if (streq(line, "upper")) {
        command_program("/bin/upper", is_background);
    } else if (streq(line, "rev")) {
        command_program("/bin/rev", is_background);
    } else if (streq(line, "hang")) {
        command_program("/bin/hang", is_background);
    } else if (streq(line, "spin")) {
        command_program("/bin/spin", is_background);
    } else if (starts_with(line, "cat ")) {
        command_cat(line + 4);
    } else if (starts_with(line, "touch ")) {
        command_touch(line + 6);
    } else if (starts_with(line, "echo ")) {
        command_echo_redirect(line);
    } else {
        write_str("comando desconhecido\n");
    }
}

static void execute_redirect(char *line, char *redirect)
{
    char *path;
    long fd;

    *redirect = '\0';
    path = skip_spaces(redirect + 1);
    trim_right(path);
    trim_right(line);

    if (path[0] == '\0') {
        write_str("redirect: nome ausente\n");
        return;
    }

    fd = syscall1(SYS_CREATE, (long)path);
    if (fd < 0) {
        write_str("redirect: falha ao criar arquivo\n");
        return;
    }

    syscall2(SYS_DUP2, 1, FD_STDOUT_SAVE);
    syscall2(SYS_DUP2, fd, 1);
    syscall1(SYS_CLOSE, fd);
    execute_simple(line, 0);
    syscall2(SYS_DUP2, FD_STDOUT_SAVE, 1);
    syscall1(SYS_CLOSE, FD_STDOUT_SAVE);
}

static void execute_pipe(char *left, char *right)
{
    int fds[2];
    long pid;

    *right = '\0';
    right++;
    left = skip_spaces(left);
    right = skip_spaces(right);
    trim_right(left);
    trim_right(right);

    if ((!starts_with(left, "cat ") && !starts_with(left, "echo ")) ||
        (!streq(right, "upper") && !streq(right, "rev"))) {
        write_str("pipe: suporte atual: cat/echo | upper/rev\n");
        return;
    }

    if (starts_with(left, "echo ") && streq(right, "rev")) {
        char *text = skip_spaces(left + 5);
        trim_right(text);
        size_t length = strlen(text);
        char *reversed = malloc(length);

        if (reversed == 0) {
            write_str("rev: falha ao alocar heap\n");
            return;
        }

        for (size_t i = 0; i < length; i++) {
            reversed[i] = text[length - i - 1];
        }
        write_buf(reversed, length);
        write_str("\n");
        free(reversed);
        return;
    }

    if (syscall1(SYS_PIPE, (long)fds) < 0) {
        write_str("pipe: falha ao criar pipe\n");
        return;
    }

    syscall2(SYS_DUP2, 1, FD_STDOUT_SAVE);
    syscall2(SYS_DUP2, fds[1], 1);
    if (starts_with(left, "cat ")) {
        command_cat_raw(left + 4);
    } else {
        command_echo_raw(left + 5);
    }
    syscall2(SYS_DUP2, FD_STDOUT_SAVE, 1);
    syscall1(SYS_CLOSE, FD_STDOUT_SAVE);
    syscall1(SYS_CLOSE, fds[1]);

    syscall2(SYS_DUP2, 0, FD_STDIN_SAVE);
    syscall2(SYS_DUP2, fds[0], 0);
    syscall1(SYS_CLOSE, fds[0]);
    pid = spawn_program(streq(right, "upper") ? "/bin/upper" : "/bin/rev");
    syscall2(SYS_DUP2, FD_STDIN_SAVE, 0);
    syscall1(SYS_CLOSE, FD_STDIN_SAVE);

    if (pid >= 0) {
        wait_pid(pid);
    }
}

static void execute_command(char *line)
{
    int is_background = detach_background_marker(line);
    char *pipe = find_pipe(line);
    char *redirect = find_redirect(line);

    if (line[0] == '\0') {
        return;
    }

    if (pipe != 0) {
        execute_pipe(line, pipe);
    } else if (redirect != 0) {
        execute_redirect(line, redirect);
    } else {
        execute_simple(line, is_background);
    }
}

static void sigint_handler(int signum)
{
    (void)signum;
    shell_interrupted = 1;
    write_str("\n[SIGINT recebido no Shell!]\n");
}

void _start(void)
{
    char line[80];

    syscall2(SYS_SIGNAL, SIGINT, (long)sigint_handler);

    write_str("PhotonOS user shell iniciado\n");

    for (;;) {
        write_str("PhotonOS /> ");
        read_line(line, sizeof(line));
        execute_command(line);
    }
}
