# PhotonOS v4.3 — Arquitetura de Ciclo de Vida de Processos

## 1. Visão Geral

O PhotonOS v4.3 introduz uma máquina de estados estrita e atômica para o gerenciador de tarefas e processos, garantindo isolamento de memória virtual, destruição segura de recursos sem Use-After-Free (UAF), eliminação de vazamentos de descritores de arquivos e reuso confiável de PIDs monotônicos.

---

## 2. Diagrama de Estados do Processo

```text
       [TASK_UNUSED] / [TASK_DEAD]
                  │
                  │ fork() / spawn() / elf_load_process()
                  ▼
            [TASK_READY] ◄───────────────┐
                  │                      │
                  │ scheduler_tick()     │ yield() / preempção / SIGCONT
                  ▼                      │
           [TASK_RUNNING] ───────────────┤
                  │                      │
         ┌────────┴────────┬─────────────┤
         │ wait/sleep      │ SIGSTOP     │
         ▼                 ▼             │
   [TASK_SLEEPING] / [TASK_STOPPED] ─────┘
   [TASK_WAITING]  /
   [TASK_BLOCKED]
         │
         │ exit() / SIGKILL / Sinal Fatal
         ▼
   [TASK_ZOMBIE] (libera memória de usuário, CR3, FDs; reparenting para PID 1; envia SIGCHLD)
         │
         │ wait() / waitpid()
         ▼
    [TASK_DEAD] (reap atômico; PID reciclado)
         │
         ▼
   [TASK_UNUSED]
```

---

## 3. Estados de Tarefa (`enum task_state`)

1. **`TASK_UNUSED` (0)**: Slot na tabela de tarefas nunca utilizado.
2. **`TASK_READY` (1)**: Tarefa apta a ser escalonada na fila Round-Robin.
3. **`TASK_RUNNING` (2)**: Tarefa atualmente em execução em uma das CPUs.
4. **`TASK_SLEEPING` (3)**: Tarefa aguardando temporizador, I/O ou stdin.
5. **`TASK_WAITING` (4)**: Tarefa pai aguardando terminação de filhos via `waitpid()`.
6. **`TASK_BLOCKED` (5)**: Tarefa bloqueada em mutex, socket ou recurso de rede.
7. **`TASK_STOPPED` (6)**: Tarefa suspensa via `SIGSTOP`/`SIGTSTP`. Não é escalonada até receber `SIGCONT`.
8. **`TASK_ZOMBIE` (7)**: Processo encerrado cujo exit status ainda não foi colhido pelo pai.
9. **`TASK_DEAD` (8)**: Processo já colhido por `waitpid()`. O slot está pronto para liberação e reuso.

---

## 4. Gerador de PID Monotônico (`generate_pid`)

Para evitar conflitos com reciclagem prematura de PID em sistemas multithread/SMP:
* O gerador utiliza um contador monotônico incremental `next_pid`.
* Antes de atribuir um PID, varre a tabela sob `task_table_lock` garantindo que nenhuma tarefa ativa ou zumbi detenha o mesmo PID.
* Slots marcados como `TASK_UNUSED` ou `TASK_DEAD` não bloqueiam a geração.

---

## 5. Terminação e Liberação de Recursos (`scheduler_terminate_task`)

Quando uma tarefa termina via `exit(status)` ou recepção de sinal fatal (`SIGKILL`, `SIGPIPE`, `SIGSEGV`, etc.):
1. **Transição Atômica**: O estado transita imediatamente para `TASK_ZOMBIE` sob `task_table_lock`.
2. **File Descriptors**: Todos os file descriptors abertos pela tarefa têm seus `ref_count` decrementados via `task_close_all_fds()`. Locks de arquivo associados são liberados.
3. **Memória de Usuário**: Todas as páginas físicas alocadas são liberadas ao PMM (`pmm_free`) e o PML4 é destruído (`vmm_destroy_address_space`). O `CR3` da tarefa retorna ao `vmm_kernel_pml4()`.
4. **Reparenting Automático**: Filhos órfãos do processo têm seu `parent_pid` reatribuído para `PID 1` (shell/init), prevenindo zumbis imortais.
5. **Notificação ao Pai**:
   * O pai é acordado caso esteja bloqueado em `wait()` (`TASK_WAIT_CHILD`).
   * Se o pai registrou um handler para `SIGCHLD`, o sinal é enfileirado atomicamente.

---

## 6. Colheita de Filhos (`sys_waitpid`)

A syscall `sys_waitpid(pid, status, options)` suporta:
* `pid == -1`: Aguarda qualquer filho.
* `pid > 0`: Aguarda filho específico.
* `options == WNOHANG`: Retorna `0` imediatamente se nenhum filho terminou, sem bloquear.
* **Colheita Atômica**: Ao localizar o filho `TASK_ZOMBIE`, copia o `exit_status`, marca o estado como `TASK_DEAD` e zera `pid` e `parent_pid` sob proteção de spinlock com IRQs desativadas (`spin_lock_irqsave`).
