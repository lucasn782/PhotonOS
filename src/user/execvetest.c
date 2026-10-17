#include "ulibc.h"

static int failed;

static void check(int condition, const char *label)
{
    printf("[EXECVE-TEST] %s %s\n", condition ? "PASS" : "FAIL", label);
    if (!condition) failed = 1;
}

int main(int argc, char **argv, char **envp)
{
    (void)argc;
    (void)argv;
    (void)envp;
    char *child_argv[] = {"/bin/argprobe", "one", "two words", "", 0};
    char *child_envp[] = {
        "USER=alice", "USERNAME=alice-long", "PHOTON_PROBE=value=with=equals",
        "EMPTY=", "FD_TEST=3", 0
    };

    int inherited_fd = open("/readme.txt", 0);
    check(inherited_fd == 3, "test descriptor opened in expected slot");

    check(execve("/bin/not-found", child_argv, child_envp) == -1,
        "failure keeps caller image alive");
    check(execve("/bin/argprobe", (char *const *)0x0000800000000000ULL,
        child_envp) == -1, "invalid argv pointer rejected");
    check(execve("/bin/argprobe", child_argv,
        (char *const *)0x0000800000000000ULL) == -1,
        "invalid envp pointer rejected");
    check(execve((const char *)0x0000800000000000ULL, child_argv,
        child_envp) == -1, "invalid path pointer rejected");

    char *bad_env[] = {"MISSING_EQUALS", 0};
    check(execve("/bin/argprobe", child_argv, bad_env) == -1,
        "malformed environment entry rejected");
    char *too_many_env[34];
    for (int i = 0; i < 33; i++) too_many_env[i] = "E=x";
    too_many_env[33] = 0;
    check(execve("/bin/argprobe", child_argv, too_many_env) == -1,
        "excessive environment vector rejected");

    char *unterminated[33];
    for (int i = 0; i < 33; i++) unterminated[i] = "x";
    check(execve("/bin/argprobe", unterminated, child_envp) == -1,
        "unterminated oversized argv rejected");

    char too_long[257];
    for (int i = 0; i < 256; i++) too_long[i] = 'x';
    too_long[256] = '\0';
    char *long_argv[] = {"/bin/argprobe", too_long, 0};
    check(execve("/bin/argprobe", long_argv, child_envp) == -1,
        "oversized argument rejected");
    check(execve("/readme.txt", child_argv, child_envp) == -1,
        "non ELF file rejected");
    check(execve("/bin/badelf", child_argv, child_envp) == -1,
        "truncated ELF header rejected");

    char *max_argv[33];
    max_argv[0] = "/bin/argprobe";
    for (int i = 1; i < 32; i++) max_argv[i] = "x";
    max_argv[32] = 0;
    int pid = fork();
    if (pid == 0) {
        execve("/bin/argprobe", max_argv, child_envp);
        exit(92);
    }
    int max_status = -1;
    check(pid > 0 && waitpid(pid, &max_status, 0) == pid && max_status == 0,
        "maximum valid argv count accepted");

    char long_valid[256];
    for (int i = 0; i < 255; i++) long_valid[i] = 'L';
    long_valid[255] = '\0';
    char *single_argv[] = {"/bin/argprobe", "only", 0};
    pid = fork();
    if (pid == 0) {
        execve("/bin/argprobe", single_argv, 0);
        exit(93);
    }
    int valid_status = -1;
    check(pid > 0 && waitpid(pid, &valid_status, 0) == pid && valid_status == 0,
        "single argument accepted");
    char *long_argv_valid[] = {"/bin/argprobe", long_valid, 0};
    pid = fork();
    if (pid == 0) {
        execve("/bin/argprobe", long_argv_valid, 0);
        exit(94);
    }
    valid_status = -1;
    check(pid > 0 && waitpid(pid, &valid_status, 0) == pid && valid_status == 0,
        "maximum length string accepted");

    char max_env_storage[32][8];
    char *max_env[33];
    for (int i = 0; i < 32; i++) {
        max_env_storage[i][0] = 'E';
        max_env_storage[i][1] = (char)('0' + (i / 10));
        max_env_storage[i][2] = (char)('0' + (i % 10));
        max_env_storage[i][3] = '=';
        max_env_storage[i][4] = 'x';
        max_env_storage[i][5] = '\0';
        max_env[i] = max_env_storage[i];
    }
    max_env[32] = 0;
    pid = fork();
    if (pid == 0) {
        execve("/bin/argprobe", single_argv, max_env);
        exit(95);
    }
    valid_status = -1;
    check(pid > 0 && waitpid(pid, &valid_status, 0) == pid && valid_status == 0,
        "maximum environment count accepted");

    pid = fork();
    if (pid == 0) {
        execve("/bin/argprobe", child_argv, child_envp);
        exit(90);
    }
    if (pid < 0) {
        check(0, "fork followed by execve");
    } else {
        int status = 0;
        check(waitpid(pid, &status, 0) == pid && status == 0,
            "fork exec child waitpid");
    }

    for (int i = 0; i < 5; i++) {
        pid = fork();
        if (pid == 0) {
            execve("/bin/argprobe", child_argv, child_envp);
            exit(91);
        }
        int status = 0;
        check(pid > 0 && waitpid(pid, &status, 0) == pid && status == 0,
            "repeated exec image cleanup");
    }

    printf("[EXECVE-TEST] %s\n", failed ? "FAILED" : "ALL PASS");
    return failed ? 1 : 0;
}
