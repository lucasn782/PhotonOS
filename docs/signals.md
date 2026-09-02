# PhotonOS v4.3 — Arquitetura de Sinais POSIX

## 1. Visão Geral

O PhotonOS v4.3 implementa um subsistema assíncrono de sinais POSIX para controle de processos e tratamento de eventos em Ring 3, com suporte a bloqueio por máscaras (`sigprocmask`), registro de tratadores (`sigaction`/`signal`), desvio de fluxo de execução em Ring 3 via trampoline de retorno (`sigreturn`), e comportamentos padrão do kernel.

---

## 2. Tabela de Sinais Suportados

| Sinal | Número | Ação Padrão | Descrição |
|---|---|---|---|
| `SIGHUP` | 1 | Terminar | Hangup detectado no terminal |
| `SIGINT` | 2 | Terminar | Interrupção por teclado (`Ctrl+C`) |
| `SIGQUIT` | 3 | Terminar | Sinal de encerramento com dump |
| `SIGILL` | 4 | Terminar | Instrução ilegal |
| `SIGTRAP` | 5 | Terminar | Trap de depuração / breakpoint |
| `SIGABRT` | 6 | Terminar | Abort (`abort()`) |
| `SIGBUS` | 7 | Terminar | Erro de barramento |
| `SIGFPE` | 8 | Terminar | Exceção aritmética de ponto flutuante / divisão por zero |
| `SIGKILL` | 9 | Terminar (Não interceptável) | Terminação forçada imediata |
| `SIGUSR1` | 10 | Terminar | Sinal customizado do usuário 1 |
| `SIGSEGV` | 11 | Terminar | Violação de segmentação / Page Fault inválido |
| `SIGUSR2` | 12 | Terminar | Sinal customizado do usuário 2 |
| `SIGPIPE` | 13 | Terminar | Escrita em pipe sem leitores ativos (*Broken Pipe*) |
| `SIGALRM` | 14 | Terminar | Temporizador de alarme |
| `SIGTERM` | 15 | Terminar | Sinal de terminação graciosa |
| `SIGCHLD` | 17 | Ignorar | Filho encerrou ou parou |
| `SIGCONT` | 18 | Continuar | Retoma processo suspenso |
| `SIGSTOP` | 19 | Parar (Não interceptável) | Suspende execução do processo |
| `SIGTSTP` | 20 | Parar | Suspensão interativa via terminal (`Ctrl+Z`) |

---

## 3. Estrutura de Estado de Sinais no TCB (`task_control_block`)

Cada processo no kernel mantém:
```c
uintptr_t signal_handlers[TASK_SIGNAL_COUNT];  /* Endereços dos handlers em Ring 3 */
uint32_t pending_signals;                      /* Bitmask de sinais enfileirados */
uint32_t blocked_signals;                      /* Bitmask de sinais bloqueados */
uint32_t active_signal;                        /* Sinal atualmente em tratamento */
struct task_signal_context signal_context;     /* Contexto salvo para sigreturn */
struct task_sigreturn_frame sigreturn_frame;   /* Frame preparado para iretq */
```

---

## 4. Entrega de Sinais e Trampoline em Ring 3

### 4.1. Pontos de Verificação
A entrega de sinais ocorre exclusivamente em momentos seguros de transição para Ring 3:
1. **Retorno de Syscall** (`scheduler_handle_syscall_signals`): Ao final do handler de syscall, antes de restaurar o contexto do usuário.
2. **Retorno de Interrupção do Scheduler** (`scheduler_tick`): No context switch, antes de alternar o PML4 e retomar o processo.

### 4.2. Fluxo do Trampoline
```text
Kernel (Ring 0)
    │
    │ 1. Salva registradores do usuário em `signal_context`
    │ 2. Prepara pilha de usuário com retorno apontando para SIGNAL_TRAMPOLINE_ADDR
    │ 3. Configura RIP = handler, RDI = signum
    ▼
Ring 3 (Handler do Usuário)
    │
    │ 4. Executa void my_handler(int signum)
    │ 5. Executa instrução `ret`
    ▼
SIGNAL_TRAMPOLINE_ADDR (0x0000008000700000ULL)
    │
    │ 6. Dispara syscall SYS_SIGRETURN (12)
    ▼
Kernel sys_sigreturn (Ring 0)
    │
    │ 7. Restaura contexto original (RIP, RSP, RFLAGS, RAX, registradores de uso geral)
    │ 8. Limpa task->active_signal
    ▼
Ring 3 (Retomada do código original)
```

---

## 5. Integração com `fork()` e `execve()`

* **`fork()`**:
  * O filho herda a tabela de tratadores (`signal_handlers`) e a máscara de sinais bloqueados (`blocked_signals`).
  * Sinais pendentes (`pending_signals`) e sinal ativo (`active_signal`) são inicializados em zero no processo filho.
* **`execve()`**:
  * Tratadores customizados são resetados para a ação padrão (`SIG_DFL`).
  * Tratadores ignorados (`SIG_IGN`) e máscaras bloqueadas (`blocked_signals`) são preservados conforme a especificação POSIX.
