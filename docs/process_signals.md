# PhotonOS v4.3 — Guia de Uso de Sinais e Syscalls

## 1. Syscalls de Sinais

### 1.1. `sigaction(int signum, const struct sigaction *act, struct sigaction *oldact)`
Permite registrar tratadores customizados ou configurar flags de sinal.
```c
struct sigaction sa;
sa.sa_handler = my_signal_handler;
sa.sa_mask = 0;
sa.sa_flags = 0;
sigaction(SIGINT, &sa, NULL);
```

### 1.2. `sigprocmask(int how, const sigset_t *set, sigset_t *oldset)`
Altera a máscara de sinais bloqueados do processo:
* `SIG_BLOCK`: Adiciona os sinais em `set` à máscara atual.
* `SIG_UNBLOCK`: Remove os sinais em `set` da máscara atual.
* `SIG_SETMASK`: Substitui a máscara atual pelo conjunto em `set`.
* **Nota**: `SIGKILL` e `SIGSTOP` não podem ser bloqueados.

### 1.3. `kill(int pid, int signum)`
Envia um sinal POSIX para o processo identificado por `pid`:
* Se `signum == SIGKILL`: O processo alvo é terminado imediatamente pelo kernel.
* Se `signum == SIGSTOP`: O processo alvo é colocado em `TASK_STOPPED`.
* Se `signum == SIGCONT`: O processo alvo é recolocado em `TASK_READY`.
* Se o processo alvo tiver um tratador para `signum`, o sinal é colocado na máscara pendente e entregue no retorno a Ring 3.

---

## 2. Exemplos de Uso no Ring 3

### 2.1. Captura de SIGCHLD
```c
#include "ulibc.h"

void chld_handler(int sig) {
    int status;
    int pid = waitpid(-1, &status, WNOHANG);
    printf("Processo filho %d finalizado com status %d\n", pid, status);
}

int main(void) {
    struct sigaction sa;
    sa.sa_handler = chld_handler;
    sa.sa_mask = 0;
    sa.sa_flags = 0;
    sigaction(SIGCHLD, &sa, NULL);

    int pid = fork();
    if (pid == 0) {
        exit(0);
    }
    // Pai continua execução normal...
}
```

### 2.2. Broken Pipe e SIGPIPE
```c
#include "ulibc.h"

int main(void) {
    int fds[2];
    pipe(fds);

    close(fds[0]); // Fecha ponta de leitura

    // Escrita na ponta de escrita gera SIGPIPE e retorna -1
    int res = write(fds[1], "dados", 5);
    if (res < 0) {
        printf("Escrita falhou com broken pipe!\n");
    }
    close(fds[1]);
}
```
