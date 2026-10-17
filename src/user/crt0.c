#include <stdint.h>

#include "ulibc.h"

extern int main(int argc, char **argv, char **envp);
uintptr_t __crt0_entry_rsp_mod16;

void crt0_main(uint64_t argc, char **argv, char **envp)
{
    environ = envp;
    exit(main((int)argc, argv, envp));
}
