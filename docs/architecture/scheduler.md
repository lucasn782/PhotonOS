# Escalonador de Tarefas (Task Scheduler)

Este documento detalha o design, as estruturas de dados, os estados das tarefas, o mecanismo de preempção por hardware, as rotinas de chaveamento de contexto e as primitivas de exclusão mútua do escalonador de tarefas preemptivo do PhotonOS.

---

## 1. Design do Escalonador e Bloco de Controle de Tarefas (TCB)

O PhotonOS v4.1 implementa um **escalonador preemptivo Round-Robin** seguro para SMP, gerenciando a concorrência de até `16` tarefas em execução simultânea (`MAX_TASKS`).

Cada fluxo de execução é representado por um Bloco de Controle de Tarefa (**TCB - Task Control Block**, declarado em `include/task.h`):

```c
struct task_control_block {
    uint64_t rsp;                       // Ponteiro de pilha salvo (RSP)
    uint64_t kernel_stack_top;          // Topo da pilha do kernel (para carregar em TSS.rsp0)
    uint64_t cr3;                       // Diretório de páginas PML4 associado ao processo
    uint32_t pid;                       // Identificador exclusivo do processo (Process ID)
    uint32_t parent_pid;                // Identificador do processo pai
    char name[16];                      // Nome descritivo da tarefa (ex: "shell")
    volatile enum task_state state;     // Estado atual da execução
    enum task_wait_reason wait_reason;  // Razão de bloqueio se a tarefa estiver dormindo
    uint64_t wait_target;               // Identificador do recurso no qual a tarefa aguarda
    int exit_status;                    // Status de saída retornado pelo processo
    uint32_t pending_signals;           // Bitmask de sinais pendentes
    uint32_t active_signal;             // Sinal atualmente em tratamento
    struct signal_context signal_context; // Contexto de registradores salvos para retorno de sinal
    uintptr_t signal_handlers[32];      // Vetor de tratadores de sinal (Ring 3 callbacks)
    uintptr_t user_physical_pages[32];  // Endereços físicos dos frames de usuário (para desalocação)
    uint32_t user_page_count;           // Quantidade de páginas de usuário alocadas
    uint64_t heap_start;                // Endereço virtual inicial do Heap de usuário
    uint64_t heap_end;                  // Limite virtual atual do Heap de usuário
    vfs_node_t *file_descriptors[8];    // Tabela de arquivos abertos (FDs)
    uint64_t fd_offsets[8];             // Offsets de leitura/escrita dos FDs
    struct cpu_registers registers;     // Cópia dos registradores da CPU
    uint32_t mutex_wait_next;           // Próxima tarefa bloqueada na fila do mutex
};
```

---

## 2. Ciclo de Estados das Tarefas

As tarefas no kernel transitam pelos seguintes estados de execução:

*   **`TASK_RUNNING`:** A tarefa está em execução ativa em um dos núcleos da CPU (BSP ou APs).
*   **`TASK_READY`:** A tarefa possui todas as condições necessárias para executar, aguardando apenas ser selecionada pela fila de Round-Robin do escalonador.
*   **`TASK_SLEEPING`:** A tarefa está suspensa temporariamente aguardando um evento (tempo decorrido, sinal de E/S em um Pipe ou buffer de teclado).
*   **`TASK_WAITING`:** A tarefa pai aguarda a terminação de um processo filho (`sys_wait`).
*   **`TASK_BLOCKED`:** A tarefa está retida no canal de rede aguardando transmissão/recepção de pacotes IP ou um lock de exclusão mútua (`mutex_lock`).
*   **`TASK_ZOMBIE`:** A tarefa chamou `sys_exit` e liberou sua memória virtual, mas seu status (`exit_status`) e TCB permanecem na tabela até que o processo pai realize a consulta via `wait()`.

```text
       [Criação]
           │
           ▼
     ┌───────────┐
  ┌─►│TASK_READY ├────────┐
  │  └─────▲─────┘        │
  │        │              ▼
  │  [Chaveamento]   ┌────────────┐     [sys_exit]
  │        │         │TASK_RUNNING├─────────────────►[TASK_ZOMBIE]
  │  ┌─────┴─────┐   └──────┬─────┘                         │
  └──┤TASK_READY │◄─────────┘                               │
     └───────────┘                                          ▼
           ▲                                            [Reclamado]
           │ [Evento / Wake]
     ┌─────┴───────────┐
     │  TASK_SLEEPING  │
     │  TASK_BLOCKED   │
     │  TASK_WAITING   │
     └─────────────────┘
```

---

## 3. Preempção e Escalonamento Round-Robin

1.  **Temporizador de Hardware (Tick):** O PIT (Programmable Interval Timer) é programado para gerar interrupções assíncronas no Vetor `0x20` a uma taxa estável de **100 Hz** (a cada 10 milissegundos).
2.  **Mecanismo de Preempção (`scheduler_tick`):**
    *   No tratamento de interrupção do timer (IRQ 0) no BSP, a CPU desvia o fluxo para o stub assembly `timer_irq_stub` em `kernel.asm`, que empilha os registradores da CPU atômicos e chama `scheduler_tick`.
    *   **Seleção de Candidato (Round-Robin):** O escalonador inicia a busca incremental a partir do índice `(current_index + 1) % task_count` na tabela de tarefas, procurando um bloco com estado `TASK_READY`.
    *   A tarefa *idle* (gereralmente de índice 0) só é selecionada caso não haja nenhuma outra tarefa do usuário ou kernel pronta para rodar.
3.  **Chaveamento de Contexto:**
    *   Seta a flag do processo em execução anterior como `TASK_READY`.
    *   Salva o valor do registrador `RSP` atual no campo `rsp` da TCB da tarefa antiga.
    *   Carrega o novo `RSP` com o ponteiro salvo da TCB do novo processo.
    *   Atualiza o topo da pilha de kernel (`kernel_stack_top`) na TSS através da chamada assembly `tss_set_rsp0`. Isso assegura que se uma interrupção ocorrer enquanto o processo executa em espaço de usuário (Ring 3), o hardware comutará de volta para a pilha de kernel correta desse processo específico.
    *   Executa a comutação de espaço de endereçamento atualizando o registrador de controle `CR3` com o diretório de páginas do novo processo (`cr3`).
    *   O stub assembly desempilha os registradores do novo processo e executa a instrução `iretq`, retomando a execução do novo código.

---

## 4. Concorrência e Primitivas de Sincronização

A tabela de controle de tarefas (`tasks`) é uma estrutura compartilhada globalmente. Ela é manipulada tanto por rotinas de interrupção assíncronas (IRQ handlers) quanto por processos e threads em Ring 0 em múltiplos núcleos (SMP).

A proteção da integridade da tabela de tarefas é implementada por meio de um **spin-lock** dedicado:

```c
static spinlock_t task_table_lock;
```

Para prevenir deadlocks induzidos por interrupções aninhadas em um mesmo núcleo:
*   Todas as manipulações críticas da tabela de tarefas são envolvidas por `spin_lock_irqsave(&task_table_lock)` e `spin_unlock_irqrestore(&task_table_lock, flags)`.
*   Para evitar a perda de contexto de flags de interrupção em concorrência SMP multicore, o PhotonOS mantém um array estático de flags de backup (`task_table_flags[256]`) indexado pelo identificador LAPIC físico de cada núcleo de processamento.
*   Isso garante que um núcleo rodando `scheduler_fork_current` ou modificando o estado de uma tarefa preserve a máscara de interrupção local (`sti`/`cli`) de forma independente e thread-safe.
