#include "ulibc.h"

int main(int argc, char **argv, char **envp)
{
    printf("[ARGPROBE] entry-rsp-mod16=%u\n",
        (unsigned int)__crt0_entry_rsp_mod16);
    if (__crt0_entry_rsp_mod16 != 8) return 2;
    printf("argc=%d\n", argc);
    for (int i = 0; i < argc; i++) printf("argv[%d]=<%s>\n", i, argv[i]);
    const char *probe = getenv("PHOTON_PROBE");
    printf("PHOTON_PROBE=<%s>\n", probe != 0 ? probe : "(null)");
    const char *fd_value = getenv("FD_TEST");
    if (fd_value != 0) {
        int fd = 0;
        for (size_t i = 0; fd_value[i] >= '0' && fd_value[i] <= '9'; i++)
            fd = fd * 10 + fd_value[i] - '0';
        struct stat fd_stat;
        int fd_ok = fstat(fd, &fd_stat) == 0 && fd_stat.st_size > 0;
        printf("[ARGPROBE] %s inherited-fd\n", fd_ok ? "PASS" : "FAIL");
        if (!fd_ok) return 3;
    }
    if (probe != 0 && argc == 4) {
        int ok = argc == 4 && argv[1][0] == 'o' &&
            argv[2][0] == 't' && argv[2][1] == 'w' && argv[3][0] == '\0' &&
            getenv("USER") != 0 && getenv("USER")[0] == 'a' &&
            getenv("USERNAME") != 0 && getenv("USERNAME")[0] == 'a' &&
            getenv("EMPTY") != 0 && getenv("EMPTY")[0] == '\0' &&
            getenv("PHOTON_PROBE")[0] == 'v' && getenv("PHOTON") == 0;
        printf("[ARGPROBE] %s argv/envp/getenv\n", ok ? "PASS" : "FAIL");
        if (!ok) return 1;
    }
    int envc = 0;
    while (envp != 0 && envp[envc] != 0) envc++;
    printf("envc=%d\n", envc);
    for (int i = 0; i < envc; i++) printf("envp[%d]=<%s>\n", i, envp[i]);
    return 0;
}
