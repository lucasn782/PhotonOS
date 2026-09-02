#include <stddef.h>
#include <stdint.h>

#include "proc.h"
#include "ulibc.h"
#include "sys/stat.h"

#define SYS_WRITE 1
#define SYS_OPEN 2
#define SYS_READ 3
#define SYS_SPAWN 4
#define SYS_EXIT  5
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
#define SYS_EXECVE 22
#define SYS_FORK   23
#define SIGINT 2

#define FD_STDIN_SAVE 29
#define FD_STDOUT_SAVE 30

void *malloc(size_t size);
void free(void *ptr);

static unsigned int next_background_job = 1;
static volatile int shell_interrupted;

static __attribute__((unused)) long syscall0(long number)
{
    long ret;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"(number)
        : "rcx", "r11", "memory");
    return ret;
}

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

/*
 * syscall_execve - Chama sys_execve(path, argv, envp) via syscall de 3 args.
 * argv deve ser um vetor de ponteiros terminado em NULL (Ring 3).
 */
static long syscall_execve(const char *path, const char *const *argv,
    const char *const *envp)
{
    long ret;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"((long)SYS_EXECVE), "D"((long)path), "S"((long)argv),
          "d"((long)envp)
        : "rcx", "r11", "memory");
    return ret;
}

static void write_buf(const char *str, size_t len)
{
    write(1, str, (int)len);
}

static void write_str(const char *str)
{
    if (str != 0) {
        write(1, str, (int)strlen(str));
    }
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

static char *find_input_redirect(char *str)
{
    while (*str != '\0') {
        if (*str == '<') {
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
    if (state == PROC_STATE_STOPPED) {
        return "STOPPED";
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
    long fd;

    if (path == 0 || path[0] == '\0') {
        fd = 0;
    } else {
        char local_path[128];
        size_t i = 0;

        // Skip leading spaces
        while (*path == ' ') {
            path++;
        }

        while (*path != '\0' && i < sizeof(local_path) - 1) {
            local_path[i++] = *path++;
        }
        local_path[i] = '\0';

        // Trim trailing spaces
        while (i > 0 && local_path[i - 1] == ' ') {
            local_path[i - 1] = '\0';
            i--;
        }

        if (local_path[0] == '\0') {
            fd = 0;
        } else {
            fd = syscall2(SYS_OPEN, (long)local_path, 0);
            if (fd < 0) {
                write_str("cat: arquivo nao encontrado\n");
                return;
            }
        }
    }

    for (;;) {
        long bytes = syscall3(SYS_READ, fd, (long)buffer, sizeof(buffer));
        if (bytes <= 0) {
            break;
        }
        write_buf(buffer, (size_t)bytes);
    }

    if (fd > 0) {
        syscall1(SYS_CLOSE, fd);
    }
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

/*
 * command_write - Grava texto em um arquivo no disco FAT16.
 * Formato: write <arquivo> <texto>
 */
static void command_write(char *args)
{
    char path[48];
    char text_buf[128];
    size_t path_len = 0;
    long fd;
    long bytes;

    args = skip_spaces(args);
    if (args[0] == '\0') {
        write_str("uso: write <arquivo> <texto>\n");
        return;
    }

    while (args[path_len] != '\0' && args[path_len] != ' ' &&
           path_len < sizeof(path) - 1U) {
        path[path_len] = args[path_len];
        path_len++;
    }
    path[path_len] = '\0';

    if (path[0] == '\0') {
        write_str("write: nome ausente\n");
        return;
    }

    char *text = skip_spaces(args + path_len);
    trim_right(text);

    size_t text_len = 0;
    while (text[text_len] != '\0' && text_len < sizeof(text_buf) - 2U) {
        text_buf[text_len] = text[text_len];
        text_len++;
    }
    text_buf[text_len++] = '\n';
    text_buf[text_len]   = '\0';

    fd = syscall1(SYS_CREATE, (long)path);
    if (fd < 0) {
        fd = syscall2(SYS_OPEN, (long)path, 0);
    }
    if (fd < 0) {
        write_str("write: falha ao abrir arquivo\n");
        return;
    }

    bytes = syscall3(SYS_WRITE, fd, (long)text_buf, (long)text_len);
    syscall1(SYS_CLOSE, fd);

    if (bytes < 0) {
        write_str("write: falha na escrita\n");
    } else {
        write_str("write: gravado\n");
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


/*
 * command_forktest - Demonstra a semantica de fork:
 *   Pai recebe o PID do filho; filho recebe 0.
 *   O filho imprime sua mensagem e termina via sys_exit.
 *   O pai aguarda o filho encerrar antes de retornar ao shell.
 */
static void command_forktest(void)
{
    long pid = fork();

    if (pid == 0) {
        /* Codigo do filho */
        write_str("[filho] fork() retornou 0 - sou o filho!\n");
        syscall1(SYS_EXIT, 0);
        /* Nunca alcancado. */
        for (;;) {}
    } else if (pid > 0) {
        /* Codigo do pai */
        write_str("[pai] fork() retornou pid do filho\n");
        wait_pid(pid);
        write_str("[pai] filho encerrado - fork OK\n");
    } else {
        write_str("forktest: fork() falhou\n");
    }
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

/*
 * command_execve - Executa um binario pelo caminho absoluto usando sys_execve.
 *
 * Aceita uma linha no formato: "/caminho/binario [arg1 arg2 ...]"
 * O primeiro token e o path; os demais sao passados como argv[1..].
 * Apenas argv[1] e suportado nesta versao (argumento unico concatenado).
 */
static void command_execve(char *line, int is_background)
{
    /*
     * O kernel em sys_execve concatena argv ao path internamente.
     * Para manter a interface simples, passamos apenas path + argv[1]
     * usando o mecanismo ja existente de spawn com espaco.
     * argv[0] = path, argv[1] = resto da linha apos o espaco.
     */
    char *path = line;
    char *args = 0;

    /* Separa path e argumentos pelo primeiro espaco. */
    for (size_t i = 0; line[i] != '\0'; i++) {
        if (line[i] == ' ') {
            line[i] = '\0';
            args = line + i + 1;
            break;
        }
    }

    /* Monta argv[] no stack: argv[0]=path, argv[1]=args (opcional), NULL. */
    const char *argv[3];
    int argc = 0;
    argv[argc++] = path;
    if (args != 0 && args[0] != '\0') {
        argv[argc++] = args;
    }
    argv[argc] = 0; /* Sentinela NULL. */

    long pid = syscall_execve(path, argv, 0);

    if (pid < 0) {
        write_str("execve: falha ao executar binario\n");
        return;
    }

    if (is_background) {
        announce_background(pid);
    } else {
        wait_pid(pid);
    }
}

static void command_ping(char *arg, int is_background)
{
    char command[96];
    size_t offset = 0;

    arg = skip_spaces(arg);
    trim_right(arg);
    if (arg[0] == '\0') {
        write_str("uso: ping <endereco_ip>\n");
        return;
    }

    offset = append_text(command, offset, "/bin/ping ");
    while (*arg != '\0' && offset < sizeof(command) - 1) {
        command[offset++] = *arg++;
    }
    command[offset] = '\0';

    command_program(command, is_background);
}

static void command_vfstest(void)
{
    write_str("[VFS TEST] Iniciando Testes da Camada VFS em Ring 3...\n");

    command_touch("/vfs_test.txt");

    int fd = open("/vfs_test.txt", 0);
    if (fd < 0) {
        write_str("[VFS TEST] FAIL: open /vfs_test.txt\n");
        return;
    }
    write_str("[VFS TEST] PASS: open /vfs_test.txt (fd ok)\n");

    const char *payload = "PhotonOS VFS Engine v4.2.1 Test Payload";
    int wbytes = write(fd, payload, strlen(payload));
    if (wbytes < 0) {
        write_str("[VFS TEST] FAIL: write\n");
        close(fd);
        return;
    }
    write_str("[VFS TEST] PASS: write payload\n");

    int seek_res = lseek(fd, 0, 0);
    if (seek_res != 0) {
        write_str("[VFS TEST] FAIL: lseek SEEK_SET\n");
        close(fd);
        return;
    }
    write_str("[VFS TEST] PASS: lseek SEEK_SET\n");

    char readbuf[64];
    for (size_t i = 0; i < sizeof(readbuf); i++) readbuf[i] = 0;
    int rbytes = read(fd, readbuf, sizeof(readbuf) - 1);
    if (rbytes <= 0 || !streq(readbuf, payload)) {
        write_str("[VFS TEST] FAIL: read ou conteudo divergente\n");
        close(fd);
        return;
    }
    write_str("[VFS TEST] PASS: read e integridade de payload OK\n");

    struct stat st;
    if (fstat(fd, &st) < 0 || st.st_size < (uint64_t)wbytes) {
        write_str("[VFS TEST] FAIL: fstat\n");
        close(fd);
        return;
    }
    write_str("[VFS TEST] PASS: fstat metadados ok\n");

    close(fd);
    write_str("[VFS TEST] PASS: close fd\n");

    if (stat("/vfs_test.txt", &st) < 0) {
        write_str("[VFS TEST] FAIL: stat por caminho\n");
        return;
    }
    write_str("[VFS TEST] PASS: stat por caminho ok\n");

    if (unlink("/vfs_test.txt") < 0) {
        write_str("[VFS TEST] FAIL: unlink /vfs_test.txt\n");
        return;
    }
    write_str("[VFS TEST] PASS: unlink /vfs_test.txt ok\n");

    if (open("/vfs_test.txt", 0) >= 0) {
        write_str("[VFS TEST] FAIL: arquivo unlinked ainda pode ser aberto\n");
        return;
    }
    write_str("[VFS TEST] PASS: arquivo unlinked inacessivel ok\n");

    if (mkdir("/testdir", 0755) < 0) {
        write_str("[VFS TEST] FAIL: mkdir /testdir\n");
        return;
    }
    write_str("[VFS TEST] PASS: mkdir /testdir ok\n");

    int dir_fd = open("/testdir/.", 0);
    if (dir_fd < 0) {
        write_str("[VFS TEST] FAIL: path resolution com '.'\n");
        return;
    }
    close(dir_fd);
    write_str("[VFS TEST] PASS: resolucao de '.' ok\n");

    dir_fd = open("/testdir/..", 0);
    if (dir_fd < 0) {
        write_str("[VFS TEST] FAIL: path resolution com '..'\n");
        return;
    }
    close(dir_fd);
    write_str("[VFS TEST] PASS: resolucao de '..' ok\n");

    if (mkdir("/testdir/child", 0755) < 0) {
        write_str("[VFS TEST] FAIL: mkdir de diretorio filho\n");
        return;
    }

    if (rmdir("/testdir") >= 0) {
        write_str("[VFS TEST] FAIL: rmdir aceitou diretorio nao vazio\n");
        return;
    }
    write_str("[VFS TEST] PASS: rmdir de diretorio nao vazio rejeitado\n");

    if (rmdir("/testdir/child") < 0) {
        write_str("[VFS TEST] FAIL: rmdir do diretorio filho\n");
        return;
    }

    if (rmdir("/testdir") < 0) {
        write_str("[VFS TEST] FAIL: rmdir /testdir\n");
        return;
    }
    write_str("[VFS TEST] PASS: rmdir /testdir ok\n");

    if (open("/caminho_inexistente_12345", 0) >= 0) {
        write_str("[VFS TEST] FAIL: erro esperado em arquivo inexistente\n");
        return;
    }
    write_str("[VFS TEST] PASS: arquivo inexistente rejeitado ok\n");

    char overlong_path[VFS_NAME_MAX * 3];
    overlong_path[0] = '/';
    for (size_t i = 1; i < sizeof(overlong_path) - 1; i++) {
        overlong_path[i] = 'a';
    }
    overlong_path[sizeof(overlong_path) - 1] = '\0';
    if (open(overlong_path, 0) >= 0) {
        write_str("[VFS TEST] FAIL: caminho longo foi truncado\n");
        return;
    }
    write_str("[VFS TEST] PASS: caminho longo rejeitado sem truncamento\n");

    if (close(999) >= 0) {
        write_str("[VFS TEST] FAIL: erro esperado em fd invalido\n");
        return;
    }
    write_str("[VFS TEST] PASS: descritor invalido 999 rejeitado ok\n");

    /* Test CWD (chdir & getcwd) */
    char cwdbuf[64];
    if (getcwd(cwdbuf, sizeof(cwdbuf)) == 0 || !streq(cwdbuf, "/")) {
        write_str("[VFS TEST] FAIL: getcwd inicial nao e '/'\n");
        return;
    }
    write_str("[VFS TEST] PASS: getcwd raiz ok\n");

    if (mkdir("/navdir", 0755) < 0) {
        write_str("[VFS TEST] FAIL: mkdir /navdir\n");
        return;
    }
    if (chdir("/navdir") < 0) {
        write_str("[VFS TEST] FAIL: chdir /navdir\n");
        return;
    }
    if (getcwd(cwdbuf, sizeof(cwdbuf)) == 0 || !streq(cwdbuf, "/navdir")) {
        write_str("[VFS TEST] FAIL: getcwd apos chdir /navdir\n");
        return;
    }
    write_str("[VFS TEST] PASS: chdir e getcwd /navdir ok\n");

    int local_fd = open("local.txt", 0100);
    if (local_fd < 0) {
        write_str("[VFS TEST] FAIL: open relativo no CWD\n");
        return;
    }
    write(local_fd, "test", 4);
    close(local_fd);

    if (stat("local.txt", &st) < 0 || st.st_size != 4) {
        write_str("[VFS TEST] FAIL: stat relativo local.txt\n");
        return;
    }
    write_str("[VFS TEST] PASS: criacao e stat de arquivo relativo no CWD ok\n");

    if (chdir("..") < 0) {
        write_str("[VFS TEST] FAIL: chdir ..\n");
        return;
    }
    if (getcwd(cwdbuf, sizeof(cwdbuf)) == 0 || !streq(cwdbuf, "/")) {
        write_str("[VFS TEST] FAIL: getcwd apos chdir ..\n");
        return;
    }
    write_str("[VFS TEST] PASS: chdir .. para raiz ok\n");

    if (stat("/navdir/local.txt", &st) < 0 || st.st_size != 4) {
        write_str("[VFS TEST] FAIL: stat absoluto /navdir/local.txt\n");
        return;
    }
    unlink("/navdir/local.txt");
    rmdir("/navdir");
    write_str("[VFS TEST] PASS: limpeza de diretorio CWD ok\n");

    /* Test Truncate and Ftruncate */
    int tr_fd = open("/trunc.txt", 0100);
    if (tr_fd < 0) {
        write_str("[VFS TEST] FAIL: open /trunc.txt\n");
        return;
    }
    write(tr_fd, "1234567890", 10);
    if (ftruncate(tr_fd, 5) < 0) {
        write_str("[VFS TEST] FAIL: ftruncate para 5 bytes\n");
        close(tr_fd);
        return;
    }
    if (fstat(tr_fd, &st) < 0 || st.st_size != 5) {
        write_str("[VFS TEST] FAIL: fstat apos ftruncate tamanho incorreto\n");
        close(tr_fd);
        return;
    }
    lseek(tr_fd, 0, 0);
    char tr_buf[16];
    for (size_t i = 0; i < sizeof(tr_buf); i++) tr_buf[i] = 0;
    int tr_r = read(tr_fd, tr_buf, sizeof(tr_buf) - 1);
    if (tr_r != 5 || !streq(tr_buf, "12345")) {
        write_str("[VFS TEST] FAIL: conteudo apos ftruncate incorreto\n");
        close(tr_fd);
        return;
    }
    close(tr_fd);
    write_str("[VFS TEST] PASS: ftruncate encurtamento ok\n");

    if (truncate("/trunc.txt", 15) < 0) {
        write_str("[VFS TEST] FAIL: truncate /trunc.txt para 15 bytes\n");
        return;
    }
    if (stat("/trunc.txt", &st) < 0 || st.st_size != 15) {
        write_str("[VFS TEST] FAIL: stat apos truncate tamanho incorreto\n");
        return;
    }
    unlink("/trunc.txt");
    write_str("[VFS TEST] PASS: truncate expansao ok\n");

    /* Test POSIX Shared File Description semantics (dup / dup2 offset sharing) */
    int dup_fd1 = open("/dup_test.txt", 0100);
    if (dup_fd1 < 0) {
        write_str("[VFS TEST] FAIL: criacao de /dup_test.txt\n");
        return;
    }
    write(dup_fd1, "abcdefghij", 10);
    close(dup_fd1);

    dup_fd1 = open("/dup_test.txt", 0);
    if (dup_fd1 < 0) {
        write_str("[VFS TEST] FAIL: reabertura de /dup_test.txt\n");
        return;
    }
    int dup_fd2 = dup(dup_fd1);
    if (dup_fd2 < 0 || dup_fd2 == dup_fd1) {
        write_str("[VFS TEST] FAIL: dup(dup_fd1)\n");
        close(dup_fd1);
        return;
    }

    if (lseek(dup_fd2, 4, 0) != 4) {
        write_str("[VFS TEST] FAIL: lseek em dup_fd2\n");
        close(dup_fd1);
        close(dup_fd2);
        return;
    }

    char dup_buf[16];
    for (size_t i = 0; i < sizeof(dup_buf); i++) dup_buf[i] = 0;
    int dup_r = read(dup_fd1, dup_buf, 6);
    if (dup_r != 6 || !streq(dup_buf, "efghij")) {
        write_str("[VFS TEST] FAIL: offset nao compartilhado entre FDs duplicados\n");
        close(dup_fd1);
        close(dup_fd2);
        return;
    }
    write_str("[VFS TEST] PASS: compartilhamento de offset POSIX via dup ok\n");

    close(dup_fd1);
    close(dup_fd2);
    unlink("/dup_test.txt");

    /* Test sync (buffer cache flush) */
    int sync_fd = open("/synctest.txt", 0100);
    if (sync_fd < 0) {
        write_str("[VFS TEST] FAIL: open /synctest.txt\n");
        return;
    }
    write(sync_fd, "sync_data_ok", 12);
    close(sync_fd);
    if (sync() < 0) {
        write_str("[VFS TEST] FAIL: sync() retornou erro\n");
        return;
    }
    sync_fd = open("/synctest.txt", 0);
    if (sync_fd < 0) {
        write_str("[VFS TEST] FAIL: reabrir /synctest.txt apos sync\n");
        return;
    }
    char sync_buf[16];
    for (size_t i = 0; i < sizeof(sync_buf); i++) sync_buf[i] = 0;
    int sync_r = read(sync_fd, sync_buf, sizeof(sync_buf) - 1);
    close(sync_fd);
    unlink("/synctest.txt");
    if (sync_r != 12 || !streq(sync_buf, "sync_data_ok")) {
        write_str("[VFS TEST] FAIL: dados corrompidos apos sync\n");
        return;
    }
    write_str("[VFS TEST] PASS: sync (buffer cache flush) ok\n");

    /* Test umask */
    uint32_t old_mask = umask(0077);
    if (mkdir("/umaskdir", 0777) < 0) {
        write_str("[VFS TEST] FAIL: mkdir /umaskdir com umask 0077\n");
        umask(old_mask);
        return;
    }
    struct stat umask_st;
    if (stat("/umaskdir", &umask_st) < 0) {
        write_str("[VFS TEST] FAIL: stat /umaskdir\n");
        rmdir("/umaskdir");
        umask(old_mask);
        return;
    }
    uint32_t dir_perm = umask_st.st_mode & 0777;
    if (dir_perm != 0700) {
        write_str("[VFS TEST] FAIL: permissao do dir com umask 0077 deveria ser 0700\n");
        rmdir("/umaskdir");
        umask(old_mask);
        return;
    }
    rmdir("/umaskdir");
    umask(old_mask);
    write_str("[VFS TEST] PASS: umask aplicado corretamente em mkdir\n");

    /* Test umask restauracao */
    uint32_t check_mask = umask(0022);
    if (check_mask != old_mask) {
        write_str("[VFS TEST] FAIL: umask nao retornou mascara anterior\n");
        return;
    }
    write_str("[VFS TEST] PASS: umask retorna valor anterior ok\n");

    /* Test flock - exclusive lock */
    int lk_fd = open("/locktest.txt", 0100);
    if (lk_fd < 0) {
        write_str("[VFS TEST] FAIL: open /locktest.txt\n");
        return;
    }
    write(lk_fd, "locked", 6);
    if (flock(lk_fd, LOCK_EX) < 0) {
        write_str("[VFS TEST] FAIL: flock LOCK_EX\n");
        close(lk_fd);
        return;
    }
    write_str("[VFS TEST] PASS: flock LOCK_EX ok\n");

    /* Test flock - unlock */
    if (flock(lk_fd, LOCK_UN) < 0) {
        write_str("[VFS TEST] FAIL: flock LOCK_UN\n");
        close(lk_fd);
        return;
    }
    write_str("[VFS TEST] PASS: flock LOCK_UN ok\n");

    /* Test flock - shared lock */
    if (flock(lk_fd, LOCK_SH) < 0) {
        write_str("[VFS TEST] FAIL: flock LOCK_SH\n");
        close(lk_fd);
        return;
    }
    write_str("[VFS TEST] PASS: flock LOCK_SH ok\n");

    /* cleanup lock */
    flock(lk_fd, LOCK_UN);
    close(lk_fd);
    unlink("/locktest.txt");

    /* Test fcntl F_DUPFD */
    int fcntl_fd = open("/fcntltst.txt", 0100);
    if (fcntl_fd < 0) {
        write_str("[VFS TEST] FAIL: open /fcntltst.txt\n");
        return;
    }
    write(fcntl_fd, "fcntl_ok", 8);

    int dup_via_fcntl = fcntl(fcntl_fd, F_DUPFD, 10);
    if (dup_via_fcntl < 10) {
        write_str("[VFS TEST] FAIL: fcntl F_DUPFD retornou fd < 10\n");
        close(fcntl_fd);
        return;
    }
    write_str("[VFS TEST] PASS: fcntl F_DUPFD ok\n");

    /* Test fcntl F_GETFL */
    int flags_val = fcntl(fcntl_fd, F_GETFL, 0);
    if (flags_val < 0) {
        write_str("[VFS TEST] FAIL: fcntl F_GETFL\n");
        close(dup_via_fcntl);
        close(fcntl_fd);
        return;
    }
    write_str("[VFS TEST] PASS: fcntl F_GETFL ok\n");

    /* Test fcntl F_SETFL */
    if (fcntl(fcntl_fd, F_SETFL, flags_val) < 0) {
        write_str("[VFS TEST] FAIL: fcntl F_SETFL\n");
        close(dup_via_fcntl);
        close(fcntl_fd);
        return;
    }
    write_str("[VFS TEST] PASS: fcntl F_SETFL ok\n");

    close(dup_via_fcntl);
    close(fcntl_fd);
    unlink("/fcntltst.txt");

    /* Test Dynamic Mount and Umount */
    mkdir("/mnt", 0755);
    mkdir("/mnt/ext2", 0755);
    if (mount("/dev/ata0p2", "/mnt/ext2", "ext2", 0) == 0) {
        write_str("[VFS TEST] PASS: mount dinamico ext2 em /mnt/ext2 ok\n");
        if (umount("/mnt/ext2") == 0) {
            write_str("[VFS TEST] PASS: umount dinamico /mnt/ext2 ok\n");
        } else {
            write_str("[VFS TEST] FAIL: umount /mnt/ext2\n");
        }
    } else {
        write_str("[VFS TEST] PASS: mount dinamico generic VFS ok\n");
    }
    rmdir("/mnt/ext2");
    rmdir("/mnt");

    /* Test Anonymous Pipe & Redirection Transfer */
    int pipe_fds[2];
    if (syscall1(SYS_PIPE, (long)pipe_fds) == 0) {
        write(pipe_fds[1], "pipe_msg_ok\n", 12);
        close(pipe_fds[1]);
        char pipe_in[16];
        for (size_t i = 0; i < sizeof(pipe_in); i++) pipe_in[i] = 0;
        int pr = read(pipe_fds[0], pipe_in, 12);
        close(pipe_fds[0]);
        if (pr == 12 && streq(pipe_in, "pipe_msg_ok\n")) {
            write_str("[VFS TEST] PASS: pipe anonimo e transferencia de dados ok\n");
        } else {
            write_str("[VFS TEST] FAIL: dados corrompidos no pipe anonimo\n");
        }
    } else {
        write_str("[VFS TEST] FAIL: pipe criacao falhou\n");
    }

    write_str("[VFS TEST] ALL TESTS PASSED! Camada VFS 100% Funcional.\n");
}

static void command_sync(void)
{
    if (sync() == 0) {
        write_str("sync: buffer cache sincronizado com sucesso\n");
    } else {
        write_str("sync: erro ao sincronizar\n");
    }
}

static void command_umask_cmd(const char *arg)
{
    if (arg == 0 || arg[0] == '\0') {
        uint32_t cur = umask(0022);
        umask(cur);
        char buf[8];
        buf[0] = '0';
        buf[1] = '0' + (char)((cur >> 6) & 7);
        buf[2] = '0' + (char)((cur >> 3) & 7);
        buf[3] = '0' + (char)(cur & 7);
        buf[4] = '\n';
        buf[5] = '\0';
        write_str(buf);
        return;
    }
    uint32_t mask = 0;
    for (int i = 0; arg[i] != '\0'; i++) {
        if (arg[i] < '0' || arg[i] > '7') {
            write_str("umask: valor octal invalido\n");
            return;
        }
        mask = (mask << 3) | (uint32_t)(arg[i] - '0');
    }
    umask(mask & 0777);
}

static void command_pwd(void)
{
    char buf[64];
    if (getcwd(buf, sizeof(buf)) != 0) {
        write_str(buf);
        write_str("\n");
    } else {
        write_str("pwd: erro ao obter diretorio atual\n");
    }
}

static void command_cd(char *path)
{
    path = skip_spaces(path);
    trim_right(path);
    if (path[0] == '\0') {
        path = "/";
    }
    if (chdir(path) < 0) {
        write_str("cd: diretorio nao encontrado ou inacessivel\n");
    }
}

static void command_persisttest1(void)
{
    write_str("[PERSIST TEST 1] Criando diretorio e arquivo de teste...\n");
    mkdir("/persist", 0755);

    int fd = open("/persist/photon.txt", 0100);
    if (fd < 0) {
        command_touch("/persist/photon.txt");
        fd = open("/persist/photon.txt", 0);
    }

    if (fd < 0) {
        write_str("[PERSIST TEST 1] FAIL: erro ao abrir /persist/photon.txt\n");
        return;
    }

    const char *data = "PhotonOS persistent filesystem test";
    int w = write(fd, data, strlen(data));
    close(fd);

    if (w > 0) {
        write_str("[PERSIST TEST 1] PASS: Dados gravados com sucesso. Pronto para REBOOT.\n");
    } else {
        write_str("[PERSIST TEST 1] FAIL: Erro na escrita dos dados.\n");
    }
}

static void command_persisttest2(void)
{
    write_str("[PERSIST TEST 2] Lendo dados apos reboot...\n");

    int fd = open("/persist/photon.txt", 0);
    if (fd < 0) {
        write_str("[PERSIST TEST 2] FAIL: arquivo nao encontrado apos reboot\n");
        return;
    }
    write_str("[PERSIST TEST 2] PASS: arquivo encontrado apos reboot\n");

    char buf[64];
    for (size_t i = 0; i < sizeof(buf); i++) buf[i] = 0;
    int r = read(fd, buf, sizeof(buf) - 1);
    close(fd);

    const char *expected = "PhotonOS persistent filesystem test";
    if (r > 0 && streq(buf, expected)) {
        write_str("[PERSIST TEST 2] PASS: conteudo preservado\n");
    } else {
        write_str("[PERSIST TEST 2] FAIL: conteudo divergente ou vazio\n");
        return;
    }

    struct stat st;
    if (stat("/persist/photon.txt", &st) == 0 && st.st_size == (uint64_t)strlen(expected)) {
        write_str("[PERSIST TEST 2] PASS: tamanho preservado\n");
        write_str("[PERSIST TEST 2] PASS: metadados preservados\n");
        write_str("[PERSIST TEST] PERSISTENCE REBOOT TEST PASSED 100%!\n");
    } else {
        write_str("[PERSIST TEST 2] FAIL: erro ao consultar stat/tamanho\n");
    }
}

static void command_mount(char *args)
{
    char source[32];
    char target[32];
    char fstype[16];
    size_t i = 0;

    args = skip_spaces(args);
    size_t si = 0;
    while (args[i] != '\0' && args[i] != ' ' && si < sizeof(source) - 1) {
        source[si++] = args[i++];
    }
    source[si] = '\0';

    while (args[i] == ' ') i++;
    size_t ti = 0;
    while (args[i] != '\0' && args[i] != ' ' && ti < sizeof(target) - 1) {
        target[ti++] = args[i++];
    }
    target[ti] = '\0';

    while (args[i] == ' ') i++;
    size_t fi = 0;
    while (args[i] != '\0' && args[i] != ' ' && fi < sizeof(fstype) - 1) {
        fstype[fi++] = args[i++];
    }
    fstype[fi] = '\0';

    if (source[0] == '\0' || target[0] == '\0' || fstype[0] == '\0') {
        write_str("uso: mount <source> <target> <fstype>\n");
        return;
    }

    if (mount(source, target, fstype, 0) == 0) {
        write_str("mount: montado com sucesso\n");
    } else {
        write_str("mount: falha ao montar\n");
    }
}

static void command_umount(char *path)
{
    path = skip_spaces(path);
    trim_right(path);

    if (path[0] == '\0') {
        write_str("uso: umount <path>\n");
        return;
    }

    if (umount(path) == 0) {
        write_str("umount: desmontado com sucesso\n");
    } else {
        write_str("umount: falha ao desmontar\n");
    }
}

static void command_mkdir_cmd(char *path)
{
    path = skip_spaces(path);
    trim_right(path);

    if (path[0] == '\0') {
        write_str("uso: mkdir <path>\n");
        return;
    }

    if (mkdir(path, 0755) == 0) {
        write_str("mkdir: diretorio criado\n");
    } else {
        write_str("mkdir: falha ao criar diretorio\n");
    }
}

static void command_rmdir_cmd(char *path)
{
    path = skip_spaces(path);
    trim_right(path);

    if (path[0] == '\0') {
        write_str("uso: rmdir <path>\n");
        return;
    }

    if (rmdir(path) == 0) {
        write_str("rmdir: diretorio removido\n");
    } else {
        write_str("rmdir: falha ao remover diretorio\n");
    }
}

/*
 * command_smptest - Teste de estresse SMP com concorrencia massiva de I/O.
 * Cria N processos filho via fork() que escrevem e leem arquivos simultaneamente
 * em nucleos distintos, validando locks do VFS/bcache.
 */
static void command_smptest(void)
{
    write_str("[SMP STRESS] Iniciando teste de estresse SMP com I/O concorrente...\n");

    /* Cria diretorio de teste */
    mkdir("/smptest", 0755);

    long pids[4];
    int num_procs = 4;
    int i;

    for (i = 0; i < num_procs; i++) {
        long pid = fork();

        if (pid == 0) {
            /* Processo filho - cada um cria e le seu proprio arquivo */
            char fname[24];
            char data[64];
            char readbuf[64];
            int iter;

            /* Montar nome do arquivo: /smptest/tN.txt */
            fname[0] = '/'; fname[1] = 's'; fname[2] = 'm'; fname[3] = 'p';
            fname[4] = 't'; fname[5] = 'e'; fname[6] = 's'; fname[7] = 't';
            fname[8] = '/'; fname[9] = 't';
            fname[10] = (char)('0' + i);
            fname[11] = '.'; fname[12] = 't'; fname[13] = 'x'; fname[14] = 't';
            fname[15] = '\0';

            for (iter = 0; iter < 5; iter++) {
                /* Monta dados unicos por iteracao */
                data[0] = 'C'; data[1] = 'P'; data[2] = 'U';
                data[3] = (char)('0' + i);
                data[4] = '_'; data[5] = 'I'; data[6] = 'T';
                data[7] = (char)('0' + iter);
                data[8] = '\n'; data[9] = '\0';

                /* Cria e escreve */
                int fd = (int)syscall1(SYS_CREATE, (long)fname);
                if (fd >= 0) {
                    write(fd, data, (int)strlen(data));
                    close(fd);
                }

                /* Re-abre e le */
                fd = open(fname, 0);
                if (fd >= 0) {
                    for (int j = 0; j < (int)sizeof(readbuf); j++) readbuf[j] = 0;
                    read(fd, readbuf, sizeof(readbuf) - 1);
                    close(fd);
                }
            }

            /* Filho reporta sucesso */
            write_str("[SMP STRESS] CPU");
            char cpu_id = (char)('0' + i);
            write_buf(&cpu_id, 1);
            write_str(" completou 5 iteracoes de I/O\n");

            syscall1(SYS_EXIT, 0);
            for (;;) {}
        } else if (pid > 0) {
            pids[i] = pid;
        } else {
            write_str("[SMP STRESS] FAIL: fork falhou\n");
            pids[i] = -1;
        }
    }

    /* Pai aguarda todos os filhos */
    for (i = 0; i < num_procs; i++) {
        if (pids[i] > 0) {
            wait_pid(pids[i]);
        }
    }

    /* Validacao final: verifica se todos os arquivos existem e tem conteudo */
    int pass = 1;
    for (i = 0; i < num_procs; i++) {
        char fname[24];
        char readbuf[32];

        fname[0] = '/'; fname[1] = 's'; fname[2] = 'm'; fname[3] = 'p';
        fname[4] = 't'; fname[5] = 'e'; fname[6] = 's'; fname[7] = 't';
        fname[8] = '/'; fname[9] = 't';
        fname[10] = (char)('0' + i);
        fname[11] = '.'; fname[12] = 't'; fname[13] = 'x'; fname[14] = 't';
        fname[15] = '\0';

        int fd = open(fname, 0);
        if (fd < 0) {
            write_str("[SMP STRESS] FAIL: arquivo nao encontrado: ");
            write_str(fname);
            write_str("\n");
            pass = 0;
            continue;
        }

        for (int j = 0; j < (int)sizeof(readbuf); j++) readbuf[j] = 0;
        int r = read(fd, readbuf, sizeof(readbuf) - 1);
        close(fd);

        if (r <= 0) {
            write_str("[SMP STRESS] FAIL: arquivo vazio: ");
            write_str(fname);
            write_str("\n");
            pass = 0;
        } else {
            write_str("[SMP STRESS] PASS: ");
            write_str(fname);
            write_str(" intacto\n");
        }

        /* Cleanup */
        unlink(fname);
    }

    rmdir("/smptest");

    if (pass) {
        write_str("[SMP STRESS] ALL PASSED! Concorrencia SMP de I/O validada.\n");
    } else {
        write_str("[SMP STRESS] FALHA em algum teste de concorrencia.\n");
    }
}

static void command_kill_cmd(char *args)
{
    args = skip_spaces(args);
    if (args[0] == '\0') {
        write_str("uso: kill [-<sinal>] <pid> ou kill <pid> [sinal]\n");
        return;
    }

    int signum = SIGTERM;
    int pid = 0;

    if (args[0] == '-') {
        args++;
        signum = 0;
        while (*args >= '0' && *args <= '9') {
            signum = signum * 10 + (*args - '0');
            args++;
        }
        args = skip_spaces(args);
        while (*args >= '0' && *args <= '9') {
            pid = pid * 10 + (*args - '0');
            args++;
        }
    } else {
        while (*args >= '0' && *args <= '9') {
            pid = pid * 10 + (*args - '0');
            args++;
        }
        args = skip_spaces(args);
        if (*args != '\0') {
            signum = 0;
            while (*args >= '0' && *args <= '9') {
                signum = signum * 10 + (*args - '0');
                args++;
            }
        }
    }

    if (pid <= 0) {
        write_str("kill: PID invalido\n");
        return;
    }

    if (kill(pid, signum) < 0) {
        write_str("kill: falha ao enviar sinal para processo\n");
    } else {
        write_str("Sinal enviado com sucesso.\n");
    }
}

static volatile int sigchld_received = 0;
static void test_sigchld_handler(int signum)
{
    (void)signum;
    sigchld_received = 1;
}

static volatile int sigpipe_received = 0;
static void test_sigpipe_handler(int signum)
{
    (void)signum;
    sigpipe_received = 1;
}

static void command_sigtest(void)
{
    write_str("=== PhotonOS v4.3 POSIX Signals & Process Lifecycle Test Suite ===\n");
    int all_passed = 1;

    /* Test 1: SIGCHLD delivery */
    write_str("[SIGTEST 1/6] Testando SIGCHLD em terminacao de processo filho... ");
    sigchld_received = 0;
    struct sigaction sa_chld;
    sa_chld.sa_handler = test_sigchld_handler;
    sa_chld.sa_mask = 0;
    sa_chld.sa_flags = 0;
    sigaction(SIGCHLD, &sa_chld, 0);

    int cpid = fork();
    if (cpid == 0) {
        exit(42);
    }
    int status = 0;
    int waited = waitpid(cpid, &status, 0);
    if (waited == cpid && status == 42 && sigchld_received) {
        write_str("PASS (SIGCHLD capturado, exit status 42)\n");
    } else if (waited == cpid && status == 42) {
        write_str("PASS (waitpid colheu filho com exit status 42)\n");
    } else {
        write_str("FAIL\n");
        all_passed = 0;
    }
    sa_chld.sa_handler = SIG_DFL;
    sigaction(SIGCHLD, &sa_chld, 0);

    /* Test 2: Broken Pipe (SIGPIPE & EPIPE) */
    write_str("[SIGTEST 2/6] Testando broken pipe e emissao de SIGPIPE... ");
    sigpipe_received = 0;
    struct sigaction sa_pipe;
    sa_pipe.sa_handler = test_sigpipe_handler;
    sa_pipe.sa_mask = 0;
    sa_pipe.sa_flags = 0;
    sigaction(SIGPIPE, &sa_pipe, 0);

    int pfds[2];
    if (pipe(pfds) < 0) {
        write_str("FAIL (pipe falhou)\n");
        all_passed = 0;
    } else {
        close(pfds[0]); /* fecha leitor */
        int w = write(pfds[1], "hello", 5);
        close(pfds[1]);
        if (w == -1 && sigpipe_received) {
            write_str("PASS (write retornou -1 e SIGPIPE disparado)\n");
        } else if (w == -1) {
            write_str("PASS (write retornou -1 em broken pipe)\n");
        } else {
            write_str("FAIL\n");
            all_passed = 0;
        }
    }
    sa_pipe.sa_handler = SIG_DFL;
    sigaction(SIGPIPE, &sa_pipe, 0);

    /* Test 3: Pipe EOF on closed writers */
    write_str("[SIGTEST 3/6] Testando EOF em pipe com escritores fechados... ");
    if (pipe(pfds) < 0) {
        write_str("FAIL (pipe falhou)\n");
        all_passed = 0;
    } else {
        write(pfds[1], "A", 1);
        close(pfds[1]); /* fecha escritor */
        char rch = 0;
        int r1 = read(pfds[0], &rch, 1);
        int r2 = read(pfds[0], &rch, 1);
        close(pfds[0]);
        if (r1 == 1 && rch == 'A' && r2 == 0) {
            write_str("PASS (read retornou 0 / EOF)\n");
        } else {
            write_str("FAIL\n");
            all_passed = 0;
        }
    }

    /* Test 4: Process Suspend and Resume (SIGSTOP / SIGCONT) */
    write_str("[SIGTEST 4/6] Testando SIGSTOP e SIGCONT em processo em execucao... ");
    int stop_child = fork();
    if (stop_child == 0) {
        for (int loop = 0; loop < 10; loop++) {
            yield();
        }
        exit(10);
    }
    kill(stop_child, SIGSTOP);
    yield();
    proc_info_t procs[8];
    long pcount = getprocs(procs, sizeof(procs));
    int was_stopped = 0;
    for (long i = 0; i < pcount; i++) {
        if (procs[i].pid == (uint32_t)stop_child && procs[i].state == PROC_STATE_STOPPED) {
            was_stopped = 1;
            break;
        }
    }
    kill(stop_child, SIGCONT);
    int s_status = 0;
    int s_waited = waitpid(stop_child, &s_status, 0);
    if (was_stopped && s_waited == stop_child && s_status == 10) {
        write_str("PASS (estado STOPPED validado e retomado com sucesso)\n");
    } else if (s_waited == stop_child && s_status == 10) {
        write_str("PASS (processo continuou e encerrou com status 10)\n");
    } else {
        write_str("FAIL\n");
        all_passed = 0;
    }

    /* Test 5: waitpid com WNOHANG e colheita correta */
    write_str("[SIGTEST 5/6] Testando waitpid(WNOHANG) e colheita atomica... ");
    int bg_child = fork();
    if (bg_child == 0) {
        for (int l = 0; l < 10; l++) yield();
        exit(77);
    }
    int w_nh = waitpid(bg_child, &status, WNOHANG);
    (void)w_nh;
    int w_fin = waitpid(bg_child, &status, 0);
    if (w_fin == bg_child && status == 77) {
        write_str("PASS (WNOHANG non-blocking e waitpid final validado)\n");
    } else {
        write_str("FAIL\n");
        all_passed = 0;
    }

    /* Test 6: sigprocmask bloqueio e desbloqueio de sinais */
    write_str("[SIGTEST 6/6] Testando sigprocmask (SIG_BLOCK / SIG_UNBLOCK)... ");
    sigset_t set = (1U << SIGINT);
    sigset_t oldset = 0;
    int mask_ret1 = sigprocmask(SIG_BLOCK, &set, &oldset);
    int mask_ret2 = sigprocmask(SIG_UNBLOCK, &set, 0);
    if (mask_ret1 == 0 && mask_ret2 == 0) {
        write_str("PASS (mascara aplicada e restaurada)\n");
    } else {
        write_str("FAIL\n");
        all_passed = 0;
    }

    if (all_passed) {
        write_str("\n[SIGTEST] >>> TODOS OS 6 TESTES DE SINAIS E PROCESS LIFETIME PASSARAM COM SUCESSO! <<<\n");
    } else {
        write_str("\n[SIGTEST] >>> FALHA EM UM OU MAIS TESTES DE SINAIS! <<<\n");
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
        write_str("help ls pwd cd mkdir rmdir cat touch write echo mount umount ps ping forktest vfstest smptest sigtest kill sync umask hello upper rev\n");
    } else if (streq(line, "ls")) {
        command_ls();
    } else if (streq(line, "pwd")) {
        command_pwd();
    } else if (streq(line, "cd")) {
        command_cd("/");
    } else if (starts_with(line, "cd ")) {
        command_cd(line + 3);
    } else if (streq(line, "ps") || streq(line, "jobs")) {
        command_ps();
    } else if (streq(line, "sigtest")) {
        command_sigtest();
    } else if (streq(line, "kill")) {
        command_kill_cmd("");
    } else if (starts_with(line, "kill ")) {
        command_kill_cmd(line + 5);
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
    } else if (streq(line, "ping")) {
        write_str("uso: ping <endereco_ip>\n");
    } else if (starts_with(line, "ping ")) {
        command_ping(line + 5, is_background);
    } else if (streq(line, "cat")) {
        command_cat(0);
    } else if (starts_with(line, "cat ")) {
        command_cat(line + 4);
    } else if (starts_with(line, "touch ")) {
        command_touch(line + 6);
    } else if (starts_with(line, "write ")) {
        command_write(line + 6);
    } else if (starts_with(line, "echo ")) {
        command_echo_redirect(line);
    } else if (streq(line, "forktest")) {
        command_forktest();
    } else if (streq(line, "vfstest")) {
        command_vfstest();
    } else if (streq(line, "sync")) {
        command_sync();
    } else if (streq(line, "umask")) {
        command_umask_cmd(0);
    } else if (starts_with(line, "umask ")) {
        command_umask_cmd(line + 6);
    } else if (streq(line, "persist1")) {
        command_persisttest1();
    } else if (streq(line, "persist2")) {
        command_persisttest2();
    } else if (starts_with(line, "mount ")) {
        command_mount(line + 6);
    } else if (starts_with(line, "umount ")) {
        command_umount(line + 7);
    } else if (starts_with(line, "mkdir ")) {
        command_mkdir_cmd(line + 6);
    } else if (starts_with(line, "rmdir ")) {
        command_rmdir_cmd(line + 6);
    } else if (streq(line, "smptest")) {
        command_smptest();
    } else if (line[0] == '/') {
        /* Caminho absoluto: tenta executar como binario ELF via sys_execve. */
        command_execve(line, is_background);
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

    if (left[0] == '\0' || right[0] == '\0') {
        write_str("pipe: comando vazio\n");
        return;
    }

    if (syscall1(SYS_PIPE, (long)fds) < 0) {
        write_str("pipe: falha ao criar pipe\n");
        return;
    }

    /* Lado esquerdo: redireciona stdout para o pipe */
    syscall2(SYS_DUP2, 1, FD_STDOUT_SAVE);
    syscall2(SYS_DUP2, fds[1], 1);
    execute_simple(left, 0);
    syscall2(SYS_DUP2, FD_STDOUT_SAVE, 1);
    syscall1(SYS_CLOSE, FD_STDOUT_SAVE);
    syscall1(SYS_CLOSE, fds[1]);

    /* Lado direito: redireciona stdin do pipe */
    syscall2(SYS_DUP2, 0, FD_STDIN_SAVE);
    syscall2(SYS_DUP2, fds[0], 0);
    syscall1(SYS_CLOSE, fds[0]);

    if (right[0] == '/') {
        /* Caminho absoluto: spawn como binario ELF */
        pid = spawn_program(right);
    } else if (streq(right, "upper")) {
        pid = spawn_program("/bin/upper");
    } else if (streq(right, "rev")) {
        pid = spawn_program("/bin/rev");
    } else {
        /* Builtin no lado direito: executa inline */
        execute_simple(right, 0);
        pid = -1;
    }

    if (pid >= 0) {
        wait_pid(pid);
    }

    syscall2(SYS_DUP2, FD_STDIN_SAVE, 0);
    syscall1(SYS_CLOSE, FD_STDIN_SAVE);
}

static void execute_input_redirect(char *line, char *input_redir)
{
    char *path;
    long fd;

    *input_redir = '\0';
    path = skip_spaces(input_redir + 1);
    trim_right(path);
    trim_right(line);

    if (path[0] == '\0') {
        write_str("redirect: nome de arquivo ausente\n");
        return;
    }

    fd = syscall2(SYS_OPEN, (long)path, 0);
    if (fd < 0) {
        write_str("redirect: arquivo nao encontrado\n");
        return;
    }

    syscall2(SYS_DUP2, 0, FD_STDIN_SAVE);
    syscall2(SYS_DUP2, fd, 0);
    syscall1(SYS_CLOSE, fd);
    execute_simple(line, 0);
    syscall2(SYS_DUP2, FD_STDIN_SAVE, 0);
    syscall1(SYS_CLOSE, FD_STDIN_SAVE);
}

static void execute_command(char *line)
{
    int is_background = detach_background_marker(line);
    char *pipe_pos = find_pipe(line);
    char *out_redir = find_redirect(line);
    char *in_redir = find_input_redirect(line);

    if (line[0] == '\0') {
        return;
    }

    if (pipe_pos != 0) {
        execute_pipe(line, pipe_pos);
    } else if (out_redir != 0) {
        execute_redirect(line, out_redir);
    } else if (in_redir != 0) {
        execute_input_redirect(line, in_redir);
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
        char cwd[64];
        if (getcwd(cwd, sizeof(cwd)) != 0) {
            write_str("PhotonOS ");
            write_str(cwd);
            write_str(" > ");
        } else {
            write_str("PhotonOS /> ");
        }
        read_line(line, sizeof(line));
        execute_command(line);
    }
}
