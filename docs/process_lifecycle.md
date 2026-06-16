# 🔄 Ciclo de Vida de Processos no PhotonOS v1.5

Este documento descreve detalhadamente o ciclo de vida dos processos no PhotonOS v1.5, com foco na implementação do mecanismo de bifurcação de processos através da chamada de sistema `sys_fork` (Syscall 23), clonagem do espaço de endereçamento virtual via VMM e PMM, montagem do contexto de execução do processo filho, e a consolidação da trindade de controle de processos POSIX (Fork, Exec, Exit).

---

## 1. Visão Geral da Infraestrutura de Processos

A partir da versão v1.5, o PhotonOS oferece suporte completo ao ciclo de vida clássico de processos de estilo Unix, permitindo que uma aplicação em espaço de usuário (Ring 3) crie novas instâncias de execução de forma preemptiva. Esta evolução assenta em três pilares fundamentais:

*   **`sys_fork` (Syscall 23)**: Duplica o processo atual, gerando um novo fluxo de execução idêntico.
*   **`sys_execve` (Syscall 23/Exec)**: Substitui a imagem de execução atual por um binário ELF do VFS.
*   **`sys_exit` (Syscall 5)**: Finaliza o processo atual e retorna seu status ao pai.

---

## 2. O Fluxo de Execução da Syscall `sys_fork`

O fluxo se inicia no espaço de usuário quando um programa invoca `fork()`. Abaixo está o caminho percorrido pela chamada no núcleo:

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

Para isolar o processo filho e garantir que suas alterações na memória não corrompam o processo pai, a função [vmm_clone_address_space](file:///c:/Users/3AM-IT/Documents/PhotonOS/src/kernel/vmm.c#L341) realiza uma clonagem profunda (*deep-copy*) do espaço virtual.

O isolamento é dividido em duas grandes regiões da tabela de páginas PML4:

### A. Higher-Half (Kernel) e Identity Map (Boot) - Cópia por Referência
*   **Entrada 0 (Identity Map de Boot)**: A entrada `0` da PML4 do pai, que cobre o mapeamento direto de estruturas iniciais de boot e o MMIO do controlador de rede e1000 (em `0xF0000000`), é copiada diretamente (por referência) para a entrada `0` do filho:
    ```c
    child_pml4[0] = parent_pml4[0];
    ```
*   **Entradas 256 a 511 (Higher-Half)**: Toda a área superior de memória correspondente ao espaço do Kernel (incluindo heap do kernel, dispositivos APIC, ACPI, etc.) é idêntica para todos os processos no sistema. Por esta razão, estas entradas da PML4 são copiadas por referência direta, garantindo que o kernel permaneça compartilhado:
    ```c
    for (int i = 256; i < 512; i++) {
        child_pml4[i] = parent_pml4[i];
    }
    ```

### B. Lower-Half (Espaço de Usuário) - Deep-Copy por Página
As entradas da PML4 de índice `1` a `255` representam o espaço de endereçamento exclusivo do usuário. Para esta faixa de memória, o sistema realiza uma varredura hierárquica completa em 4 níveis (PML4 → PDPT → PD → PT) para encontrar todas as páginas físicas mapeadas com o bit de privilégio de usuário (`VMM_USER`).

Quando uma página virtual de usuário válida é encontrada no processo pai, o seguinte fluxo de cópia profunda é executado:
1.  **Alocação Física**: O kernel aloca um novo frame físico de 4096 bytes via PMM utilizando `pmm_alloc()`.
2.  **Cópia de Conteúdo**: O conteúdo do frame físico do pai é inteiramente copiado para o novo frame físico do filho.
3.  **Mapeamento**: O novo frame do filho é mapeado no novo PML4 do filho na mesma página virtual correspondente (`virt`), herdando as mesmas permissões e flags de controle (ex: `PAGE_PRESENT`, `PAGE_WRITABLE`, `PAGE_USER`).

---

## 4. Montagem do `interrupt_task_frame` na Kernel Stack do Filho

Ao criar o novo processo filho, o escalonador reserva uma pilha de kernel exclusiva (`child_kstack`) para lidar com as interrupções e trocas de contexto do filho.

Para permitir que o filho comece a executar a partir do ponto exato onde a chamada de sistema `fork` foi efetuada no pai, o kernel constrói manualmente um frame de interrupção fake no topo da pilha de kernel do filho. Este frame é representado pela estrutura `interrupt_task_frame`.

### Estrutura do Frame na Pilha do Filho
O frame é montado no endereço `stack_top - sizeof(struct interrupt_task_frame)` e preenchido com o estado capturado da CPU do pai via `syscall_frame`:

*   **Registradores de Segmento**: O kernel define `ss` como `USER_DATA_SELECTOR` e `cs` como `USER_CODE_SELECTOR`.
*   **Instruções e Pilha**: `user_rsp` e `rip` são copiados diretamente de `user_rsp` e `rcx` do `syscall_frame` (que armazenavam o ponteiro de pilha do usuário e o endereço de retorno da instrução `syscall` do pai).
*   **Registradores de Propósito Geral**: Copiados do pai para manter o estado dos registradores intacto (`rbx`, `rcx`, `rdx`, `rsi`, `rdi`, `rbp`, `r8` até `r15`).

### A Regra de Ouro da Bifurcação (RAX)
A distinção crucial entre o processo pai e o processo filho após a bifurcação ocorre no registrador **RAX**, que armazena o valor de retorno das funções no padrão de chamada x86_64:

1.  **Processo Pai**: O retorno da função [scheduler_fork_current](file:///c:/Users/3AM-IT/Documents/PhotonOS/src/kernel/scheduler.c#L836) na thread do pai é o PID gerado para o filho (um inteiro positivo diferente de zero). O escalonador retorna esse valor diretamente no fluxo normal da syscall do pai.
2.  **Processo Filho**: Durante a montagem do frame de interrupção na pilha do filho, o kernel força explicitamente o registrador RAX do filho a zero:
    ```c
    child_frame->rax = 0;
    ```
    Quando o escalonador faz a troca de contexto para o filho pela primeira vez via `scheduler_tick`, o registrador `cr3` é carregado com `child_pml4`, o topo da pilha de kernel do filho é ajustado na TSS, e o fluxo entra em `timer_irq_stub`. O stub faz o desempilhamento (*pop*) dos registradores da pilha do filho e executa a instrução `iretq`. A CPU transiciona para Ring 3 retornando zero ao chamador.

---

## 5. Mapeamento e Correção do Ciclo de vida (SYS_EXIT ID 5)

Antes da versão v1.5, havia desalinhamentos nos identificadores das chamadas de sistema entre o espaço de usuário e o espaço de kernel. Para restabelecer a consistência do sistema de arquivos e controle de processos, os seguintes ajustes foram consolidados:

*   **Identificador Consolidado**: A chamada de sistema para encerramento de processos, `SYS_EXIT`, foi firmada definitivamente na posição **5** (`SYS_EXIT = 5`).
*   **Alinhamento na ulibc e Kernel**: O cabeçalho e bibliotecas de usuário (`ulibc.c`, `shell.c`, `hello.c`, etc.) e a tabela de despacho do kernel (`kernel.c`) compartilham o mesmo ID:
    ```c
    #define SYS_EXIT 5
    ```
*   **Garantia de Liberação de Recursos**: A execução da syscall `SYS_EXIT` encaminha a limpeza ao scheduler, liberando as páginas de usuário alocadas através de `release_user_pages` e destruindo a tabela de páginas via `vmm_destroy_address_space`. O processo transiciona para o estado `TASK_ZOMBIE` até que o processo pai o colete através de `sys_wait` (Syscall 7/Wait), evitando vazamentos de memória e frames órfãos no PMM.
