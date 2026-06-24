# Ciclo de Vida de Processos e IPC no PhotonOS v2.0

Este documento descreve detalhadamente o ciclo de vida dos processos no PhotonOS v2.0, incluindo a implementação do mecanismo de bifurcação de processos (`sys_fork`), clonagem do espaço de endereçamento virtual (VMM/PMM), contexto de execução do filho, consolidação da trindade POSIX (Fork, Exec, Exit) e a especificação de sincronização atômica em IPC (Pipes).

---

## 1. Visão Geral da Infraestrutura de Processos

O PhotonOS v2.0 oferece suporte nativo ao ciclo de vida clássico de processos Unix, permitindo que processos de espaço de usuário (Ring 3) gerenciem novas instâncias de execução concorrente através de três pilares:

*   **`sys_fork` (Syscall 23)**: Duplica o processo atual, gerando um novo fluxo de execução idêntico.
*   **`sys_execve` (Syscall 11/Exec)**: Substitui a imagem de execução atual por um binário ELF carregado do VFS.
*   **`sys_exit` (Syscall 5)**: Finaliza o processo atual e retorna seu status ao pai.

---

## 2. O Fluxo de Execução da Syscall `sys_fork`

O fluxo se inicia no espaço de usuário quando um programa invoca `fork()`. A chamada de sistema transiciona o controle para o núcleo:

```
[Espaço de Usuário]
       ↓ fork() (Invoca a Syscall 23)
[stub ASM / syscall_entry]
       ↓ Salva o estado da CPU em struct syscall_frame no Kernel Stack do Pai
[kernel.c / syscall_handler]
       ↓ Identifica SYS_FORK (23) e chama sys_fork(arg6) (arg6 = ponteiro do syscall_frame)
[scheduler.c / scheduler_fork_current]
       ├─ 1. Clona espaço de endereçamento (vmm_clone_address_space)
       ├─ 2. Aloca nova Kernel Stack para o Filho
       ├─ 3. Obtém slot na TCB e copia TCB do Pai
       ├─ 4. Constrói interrupt_task_frame no topo da Kernel Stack do Filho
       └─ 5. Define RAX = 0 no frame do Filho e insere no Escalonador
```

---

## 3. Clonagem do Espaço de Endereçamento (`vmm_clone_address_space`)

Para isolar o processo filho e garantir que suas alterações na memória não corrompam o processo pai, a rotina realiza uma clonagem profunda (*deep-copy*) do espaço virtual de endereços.

O isolamento é dividido em duas grandes regiões da tabela de páginas PML4:

### A. Higher-Half (Kernel) e Identity Map (Boot) - Cópia por Referência
*   **Entrada 0 (Identity Map de Boot)**: A entrada `0` da PML4 do pai, que cobre o mapeamento direto de estruturas de boot e o MMIO do controlador de rede e1000, é copiada diretamente (por referência) para a entrada `0` do filho:
    ```c
    child_pml4[0] = parent_pml4[0];
    ```
*   **Entradas 256 a 511 (Higher-Half)**: Toda a área superior correspondente ao Kernel (heap do kernel, MMIO de dispositivos, APIC, etc.) é compartilhada entre todos os processos. Essas entradas são copiadas por referência direta:
    ```c
    for (int i = 256; i < 512; i++) {
        child_pml4[i] = parent_pml4[i];
    }
    ```

### B. Lower-Half (Espaço de Usuário) - Deep-Copy por Página
As entradas de índice `1` a `255` representam o espaço virtual do usuário. O sistema realiza uma varredura hierárquica completa em 4 níveis (PML4 → PDPT → PD → PT) para encontrar todas as páginas físicas mapeadas com o bit de privilégio de usuário (`VMM_USER`).

Quando uma página virtual de usuário válida é encontrada no processo pai:
1.  **Alocação Física**: O kernel aloca um novo frame físico de 4096 bytes via PMM (`pmm_alloc()`).
2.  **Cópia de Conteúdo**: O conteúdo do frame do pai é inteiramente copiado para o novo frame físico do filho.
3.  **Mapeamento**: O novo frame é mapeado na tabela de páginas do filho no mesmo endereço virtual correspondente (`virt`), herdando as flags originais (ex: `PAGE_PRESENT`, `PAGE_WRITABLE`, `PAGE_USER`).

---

## 4. Montagem do `interrupt_task_frame` na Kernel Stack do Filho

Ao criar o novo processo filho, o escalonador reserva uma pilha de kernel exclusiva (`child_kstack`) para lidar com interrupções e trocas de contexto.

Para permitir que o filho comece a executar a partir do ponto exato onde a chamada de sistema `fork` foi efetuada no pai, o kernel constrói manualmente um frame de interrupção fake no topo da pilha de kernel do filho, representado pela estrutura `interrupt_task_frame`.

### Estrutura do Frame na Pilha do Filho
O frame é montado no endereço `stack_top - sizeof(struct interrupt_task_frame)` e preenchido com o estado capturado da CPU do pai:

*   **Registradores de Segmento**: O kernel define `ss` como `USER_DATA_SELECTOR` e `cs` como `USER_CODE_SELECTOR`.
*   **Instruções e Pilha**: `user_rsp` e `rip` são copiados diretamente de `user_rsp` e `rcx` do `syscall_frame` (que armazenavam o ponteiro de pilha do usuário e o endereço de retorno da instrução `syscall` do pai).
*   **Registradores de Propósito Geral**: Copiados do pai para manter o estado dos registradores intacto (`rbx`, `rcx`, `rdx`, `rsi`, `rdi`, `rbp`, `r8` até `r15`).

### A Regra de Ouro da Bifurcação (RAX)
A distinção crucial entre o processo pai e o processo filho após a bifurcação ocorre no registrador **RAX**, que armazena o valor de retorno das funções:

1.  **Processo Pai**: O retorno da função `scheduler_fork_current` é o PID gerado para o filho (um inteiro positivo diferente de zero). O escalonador retorna esse valor diretamente no fluxo normal da syscall do pai.
2.  **Processo Filho**: Durante a montagem do frame de interrupção na pilha do filho, o kernel força explicitamente o registrador RAX do filho a zero:
    ```c
    child_frame->rax = 0;
    ```
    Quando o escalonador faz a troca de contexto para o filho pela primeira vez, o registrador `cr3` é carregado com `child_pml4`, o topo da pilha de kernel do filho é ajustado na TSS, e o fluxo transiciona para o espaço de usuário através de `timer_irq_stub` / `iretq` retornando zero ao chamador.

---

## 5. Mapeamento e Correção do Ciclo de Vida (SYS_EXIT ID 5)

*   **Identificador Consolidado**: A chamada de sistema para encerramento de processos, `SYS_EXIT`, está firmada definitivamente na posição **5** (`SYS_EXIT = 5`).
*   **Alinhamento na ulibc e Kernel**: O cabeçalho de syscalls do usuário (`ulibc.c`, `shell.c`, etc.) e a tabela de despacho do kernel (`kernel.c`) compartilham o mesmo identificador.
*   **Garantia de Liberação de Recursos**: A execução da syscall `SYS_EXIT` encaminha a limpeza ao scheduler, liberando as páginas de usuário alocadas através de `release_user_pages` e destruindo a tabela de páginas via `vmm_destroy_address_space`. O processo transiciona para o estado `TASK_ZOMBIE` até que o processo pai o colete através de `sys_wait` (Syscall 7/Wait), evitando vazamentos de memória no PMM.

---

## 6. Sincronização de IPC e Atomicidade em Pipes

Para garantir que a comunicação inter-processos via Pipes (por exemplo, na execução de encadeamentos de comandos no `/bin/shell` do tipo `cat file.txt | upper`) funcione sem corrupção de dados ou condições de corrida, o PhotonOS v2.0 implementa blindagem de segurança nas rotinas de leitura e escrita.

### A. Controle Estrito de Concorrência
As rotinas `pipe_read` e `pipe_write` operam sobre uma estrutura de buffer circular (`struct pipe_buffer`) protegida por um mecanismo de exclusão mútua:
```c
struct pipe_buffer {
    mutex_t lock;
    uint8_t data[PIPE_BUFFER_SIZE];
    uint32_t read_pos;
    uint32_t write_pos;
    uint32_t count;
};
```

### B. Evitando Race Conditions e Deadlocks no Escalonador
*   **Rotina `pipe_read`**:
    1.  Adquire o bloqueio exclusivo de exclusão mútua (`mutex_lock(&pipe->lock)`).
    2.  Se houver dados disponíveis (`pipe->count > 0`), extrai os bytes, atualiza a posição de leitura (`read_pos`) e decrementa o contador.
    3.  Se nenhum dado puder ser lido (`pipe->count == 0`):
        *   Libera o lock do mutex.
        *   Bloqueia o processo atual chamando `scheduler_sleep_current(TASK_WAIT_PIPE_READ, pipe)`, instruindo o escalonador a suspender a tarefa até que dados sejam gravados no buffer.
    4.  Caso contrário, se dados foram lidos com sucesso, acorda possíveis escritores bloqueados via `scheduler_wake_pipe_writers(pipe)` antes de liberar o lock.

*   **Rotina `pipe_write`**:
    1.  Adquire o lock do buffer (`mutex_lock(&pipe->lock)`).
    2.  Enquanto houver espaço no buffer (`pipe->count < PIPE_BUFFER_SIZE`), copia os dados para o buffer, incrementa a posição de escrita (`write_pos`) e atualiza o contador.
    3.  Se nenhum dado puder ser escrito (buffer cheio):
        *   Libera o lock do mutex.
        *   Bloqueia o processo atual chamando `scheduler_sleep_current(TASK_WAIT_PIPE_WRITE, pipe)`, suspendendo o processo de escrita até que espaço seja liberado pelos leitores.
    4.  Se dados foram escritos com sucesso, acorda os leitores bloqueados via `scheduler_wake_pipe_readers(pipe)` antes de liberar o lock.

Essa sincronização atômica baseada no par Mutex + Sleep/Wake isola completamente as transições de estado do buffer circular das interrupções do Pit (PIT Timer IRQ 0), prevenindo deadlocks assíncronos e garantindo integridade de I/O em pipelines de shell.
