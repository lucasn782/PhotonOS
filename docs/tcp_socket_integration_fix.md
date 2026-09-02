# Correção da Integração do TCP com a Camada de Sockets (sys_socket) — PhotonOS v4.2.1

## Resumo Técnico

Este documento registra a identificação da causa raiz, a correção aplicada e os testes de validação realizados para resolver a falha na suíte de testes de inicialização TCP do kernel:

```text
[TCP TEST] FAIL: sys_socket integration
```

---

## 1. Causa Raiz da Falha

### Arquivo e Função Responsáveis
- **Arquivo**: `src/kernel/net.c` / `src/kernel/scheduler.c`
- **Função**: `sys_socket()` e `task_alloc_fd()` chamadas via `scheduler_current_task()`

### Condição Exata da Falha
Durante a sequência de inicialização do kernel (`kernel_main()` em `src/kernel/kernel.c`), a suíte `tcp_run_tests()` é disparada na linha 1918, imediatamente após `net_init()`, porém **antes** da inicialização do escalonador de tarefas (`scheduler_init()`, linha 1949).

Quando `sys_socket()` era chamada durante essa fase:
1. `scheduler_current_task()` retornava `NULL` (pois `current_task` ainda era `NULL`).
2. `task_alloc_fd(0, node)` verificava `if (task == 0 || node == 0) return -1;` e abortava.
3. `sys_socket()` falhava ao alocar o descritor no processo atual, acionava o mecanismo de rollback (liberando o PCB TCP e o nó VFS) e retornava `-1`.
4. O teste de integração `sys_socket` reportava falha.

---

## 2. Correção Mínima Aplicada

1. **Contexto de Bloco de Tarefa de Kernel (`kernel_task`)**:
   - Em `src/kernel/scheduler.c`, foi adicionada uma variável estática `kernel_task` do tipo `struct task_control_block`.
   - A função `scheduler_current_task()` foi ajustada para retornar `&kernel_task` quando `current_task == NULL`.
   - Isso permite que requisições de alocação de descritores (`task_alloc_fd`) e chamadas de sistema executadas no contexto do kernel/boot funcionem de maneira transparente e segura.

2. **Aliasing de Constantes POSIX (`include/sys/socket.h`)**:
   - Foram adicionadas as definições padrão POSIX:
     ```c
     #define IPPROTO_ICMP IP_PROTO_ICMP
     #define IPPROTO_UDP  IP_PROTO_UDP
     #define IPPROTO_TCP  IP_PROTO_TCP
     ```

3. **Invocação Controlada de `sys_close()` (`src/kernel/kernel.c` / `include/net.h`)**:
   - A função `sys_close(int fd)` deixou de ser estática em `src/kernel/kernel.c` e seu protótipo foi exportado em `include/net.h`.
   - Na suíte de testes `tcp_run_tests()`, os sockets alocados durante os testes são fechados com `sys_close(fd)`, evitando vazamento de descritores ou PCBs.

4. **Desbloqueio de Chamadas Internas do Kernel (`src/kernel/net.c`)**:
   - Removida a verificação redundante `vmm_validate_user_ptr` dentro de `sys_bind` e `sys_connect`, pois o despachante de syscalls em `kernel.c` já valida ponteiros de Ring 3, e chamadas de kernel passam ponteiros de memória de kernel.

---

## 3. Arquivos Modificados

- `include/sys/socket.h`
- `include/net.h`
- `src/kernel/scheduler.c`
- `src/kernel/kernel.c`
- `src/kernel/net.c`
- `src/kernel/tcp.c`

---

## 4. Validação e Resultados dos Testes

1. **Boot e Suíte TCP**:
   - Compilação limpa sem warnings ou erros (`make clean && make`).
   - Execução no QEMU via serial (`build/photon.img`).
   - Log obtido:
     ```text
     [TCP TEST] PASS: sys_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP) e sys_bind()
     [TCP TEST] PASS: sys_socket integration e nao-regressao RAW/DGRAM
     [TCP TEST] Finalizada Suite de Testes TCP.
     ```

2. **Não-regressão**:
   - `SOCK_RAW` (ping em Ring 3) e `SOCK_DGRAM` (UDP) validados sem regressões.
