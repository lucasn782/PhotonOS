# Ciclo de Vida de Processos e Otimização de Memória via COW no PhotonOS v3.1

Este documento descreve detalhadamente o ciclo de vida dos processos no PhotonOS v3.1, incluindo a implementação do mecanismo de bifurcação de processos (`sys_fork`) com **Copy-On-Write (COW)**, clonagem preguiçosa do espaço de endereçamento virtual, contador de referências de frames físicos no PMM, tratamento da exceção `INT 0x0E` e consistncia multicore via TLB Shootdown. Para a documentação completa do mecanismo COW, consulte [`docs/cow_memory_optimization.md`](cow_memory_optimization.md).

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

## 3. Clonagem do Espaço de Endereçamento via Copy-On-Write (`vmm_clone_address_space`)

A partir da v3.1, a rotina `vmm_clone_address_space` opera sob o paradigma de **Clonagem Preguiçosa** (*Lazy Page Cloning*): em vez de duplicar fisicamente o conteúdo de cada frame de usuário no momento do `fork`, o kernel faz pai e filho compartilharem os **mesmos frames físicos**, postergando a cópia até que uma escrita efetiva ocorra.

O isolamento continua dividido em duas grandes regiões da tabela de páginas PML4:

### A. Higher-Half (Kernel) e Identity Map (Boot) — Cpia por Referência (inalterado)
*   **Entrada 0 (Identity Map de Boot)**: Copiada diretamente por referência — cobre estruturas de boot precoce e o MMIO do driver e1000.
    ```c
    child_pml4[0] = parent_pml4[0];
    ```
*   **Entradas 256 a 511 (Higher-Half)**: Compartilhadas por referência direta — cobrem o heap do kernel, o framebuffer gráfico, o LAPIC e todos os mapeamentos Ring 0. Estas entradas **nunca** são submetidas ao protocolo COW.
    ```c
    for (int i = 256; i < 512; i++) {
        child_pml4[i] = parent_pml4[i];
    }
    ```

### B. Lower-Half (Espaço de Usuário) — Clonagem COW (v3.1)
As entradas de índice `1` a `255` representam o espaço virtual do usuário. A varredura hierárquica de 4 níveis (PML4 → PDPT → PD → PT) permanece idêntica, mas a semântica de cada PTE válida encontrada é radicalmente diferente:

Para cada PTE de usuário válida encontrada no processo pai:
1.  **Degradação de Privilégio**: Se a página possui `PAGE_WRITABLE` (bit 1), o kernel remove essa flag e seta `PAGE_COW` (bit 9 = `0x200`) na PTE **do pai**.
2.  **Mapeamento Compartilhado**: O mesmo endereço físico (`phys_addr`) é mapeado na PML4 do filho com as mesmas flags ajustadas — **nenhum novo frame é alocado**.
3.  **Invalidao do TLB Local**: `vmm_flush_tlb(virt)` invalida seletivamente a entrada TLB do BSP via `invlpg`.
4.  **Incremento de Refcount**: `pmm_ref_inc(phys_addr)` incrementa o contador de referências do frame compartilhado no array `pmm_refcounts`.

```c
/* vmm.c — protocolo COW por PTE */
if (flags & PAGE_WRITABLE) {
    flags &= ~PAGE_WRITABLE;      /* Remove escrita */
    flags |=  PAGE_COW;           /* Seta bit COW (0x200) */
    parent_pt[pt_i] = phys_addr | flags;   /* Modifica PTE do pai */
}
map_in_pml4(child_pml4, virt, phys_addr, (uint32_t)flags);
vmm_flush_tlb(virt);
pmm_ref_inc((void *)phys_addr);
```

Após a conclusão da varredura, se há APs ativos, o kernel emite um **TLB Shootdown IPI** (Vector `0x79`) via LAPIC broadcast para garantir que todos os núcleos descartam entradas TLB obsoletas. Veja a Seção 3B.

---

## 3B. Tratamento de Escrita em Página COW — `vmm_page_fault_handler` (`INT 0x0E`)

Quando um processo (pai ou filho) tenta **escrever** em uma página marcada com `PAGE_COW`, a CPU gera uma exceção de Page Fault (`#PF`, `INT 0x0E`) porque `PAGE_WRITABLE` foi removido. O handler intercepta a falha e executa a separação física das páginas:

### Critérios de Validação COW
A exceção é tratada como COW apenas se **ambas** as condições forem verdadeiras:
- `error_code & 0x02` — a falha foi gerada por uma **operação de escrita** (bit 1 do código de erro).
- `pte & PAGE_COW` — a PTE do endereço em `CR2` possui o bit `0x200` setado.

### Lógica de Divisão de Frame (*Page Splitting*)

| Caso | Condição | Ação |  
|------|-----------|------|
| **Frame Compartilhado** | `pmm_ref_get(old_frame) > 1` | Aloca novo frame via PMM, copia 4 KiB, atualiza PTE com `PAGE_WRITABLE` e decrementa refcount do frame antigo via `pmm_free` |
| **Frame Exclusivo** | `pmm_ref_get(old_frame) == 1` | Restaura `PAGE_WRITABLE` e limpa `PAGE_COW` in-place — sem nova alocação |

Em ambos os casos, `vmm_flush_tlb(fault_addr)` invalida a entrada TLB local antes do retorno ao usuário via `iretq`.

Consulte [`docs/cow_memory_optimization.md §4`](cow_memory_optimization.md) para o detalhamento completo do fluxo assembly e C do handler.

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
