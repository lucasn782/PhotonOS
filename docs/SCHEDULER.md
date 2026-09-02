# PhotonOS v4.3 — Arquitetura do Escalonador (Scheduler)

## 1. Visão Geral

O escalonador do PhotonOS v4.3 é um escalonador preemptivo Round-Robin adaptado para arquiteturas x86_64 SMP (Multi-Core), operando com suporte completo a estados de processos, suspensão/retomada por sinais (`SIGSTOP`/`SIGCONT`), espera bloqueante por filhos (`waitpid`/`WNOHANG`), e entrega assíncrona de sinais em transições de volta ao Ring 3.

---

## 2. Estrutura e Sincronização SMP

* **Tabela de Tarefas**: Vetor estático protegido por spinlock `task_table_lock`.
* **Proteção de IRQ**: Funções que manipulam a tabela utilizam `spin_lock_irqsave(&task_table_lock)` e `spin_unlock_irqrestore(&task_table_lock, flags)` para evitar deadlocks causados por interrupções de timer durante manipulação de estruturas.
* **Isolamento de Pilha**: Cada CPU do SMP executa o escalonamento independentemente via interrupção do timer (PIT / Local APIC), chaveando a pilha do kernel (`kernel_stack_top`) na TSS (`tss_set_rsp0`) e o espaço de endereçamento (`CR3`).

---

## 3. Escalonamento e Estados Suportados

Na função `scheduler_tick(uint64_t current_rsp)`:
1. Salva o `current_rsp` no TCB do processo atual.
2. Se o processo atual estava `TASK_RUNNING`, transita para `TASK_READY`.
3. Percorre a tabela de tarefas selecionando o próximo candidato `TASK_READY`.
4. Tarefas nos estados `TASK_SLEEPING`, `TASK_WAITING`, `TASK_BLOCKED`, `TASK_STOPPED`, `TASK_ZOMBIE`, `TASK_DEAD` e `TASK_UNUSED` são ignoradas pelo laço de escalonamento.
5. Se nenhuma tarefa de usuário estiver pronta, a CPU executa a thread `idle` (`scheduler_idle_thread`), que opera em loop com `sti; hlt`.

---

## 4. Entrega de Sinais no Escalonador

Antes de alternar para a tarefa selecionada:
* Se a tarefa possuir sinais pendentes (`pending_signals & ~blocked_signals != 0`) e não estiver com sinal ativo (`active_signal == 0`):
  * O espaço de endereçamento da tarefa é ativado temporariamente (`vmm_switch_address_space`).
  * `deliver_pending_signal_unlocked(candidate)` configura o frame do stack e altera o RIP para o tratador de sinal do usuário.
