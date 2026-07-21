# Fluxo de System Calls

Este documento especifica o caminho de execução, os layouts de registradores da CPU e a inicialização de MSRs para transições rápidas do espaço de usuário para o kernel usando a interface `syscall`/`sysret` do x86_64.

---

## 1. Configuração da Interface de System Call

O PhotonOS utiliza o mecanismo de `syscall` / `sysret` assistido por hardware do x86_64 em vez de interrupções de software legadas (ex.: `int 0x80`).

Durante `syscall_init()` em `kernel.c`, os seguintes Model Specific Registers (MSRs) são inicializados:

1. **`IA32_STAR` (`0xC0000081`)**: Especifica os segmentos usados na transição:
   - Bits 32-47: Seletor de Código do Kernel `0x08` (e Stack `0x10`).
   - Bits 48-63: Base do Usuário `0x18` (resultando em Código `0x2B` e Stack `0x23`).
2. **`IA32_LSTAR` (`0xC0000082`)**: Configurado com o endereço de `syscall_entry` em `src/boot/kernel.asm`, que serve como stub de entrada para todas as syscalls.
3. **`IA32_FMASK` (`0xC0000084`)**: Configurado para `0x200` para limpar automaticamente o flag de interrupção (`RFLAGS.IF`) na entrada, prevenindo interrupções até que a pilha do kernel esteja seguramente estabelecida.

---

## 2. Convenções de Registradores (AMD64 System V ABI)

Quando uma aplicação do espaço de usuário executa a instrução `syscall`:

- **Parâmetros de Entrada**:
  - `RAX`: Número da System Call.
  - Argumentos: `RDI`, `RSI`, `RDX`, `R10`, `R8`, `R9`.
- **Preservação pela CPU**:
  - `RCX`: Salva o ponteiro de instrução de retorno do espaço de usuário (`RIP`).
  - `R11`: Salva os flags de status do espaço de usuário (`RFLAGS`).
- **Valor de Retorno**:
  - `RAX`: Resultado da system call retornado ao espaço de usuário.

---

## 3. Stub de Entrada do Kernel (`syscall_entry`)

O handler de entrada em `src/boot/kernel.asm` realiza a transição:

1. **Troca de Ponteiro de Pilha**: Utiliza a instrução `swapgs` para trocar o registrador GS do espaço de usuário com a base GS do kernel (contendo ponteiros TCB), e alterna `RSP` da pilha do espaço de usuário para a pilha do kernel da tarefa.
2. **Salvar Contexto**: Empilha registradores para construir um `struct syscall_frame`:
   ```c
   struct syscall_frame {
       uint64_t r9;
       uint64_t r8;
       uint64_t r10;
       uint64_t rdx;
       uint64_t rsi;
       uint64_t rdi;
       uint64_t r15;
       uint64_t r14;
       uint64_t r13;
       uint64_t r12;
       uint64_t rbp;
       uint64_t rbx;
       uint64_t r11; // RFLAGS salvo
       uint64_t rcx; // RIP salvo
       uint64_t user_rsp;
   };
   ```
3. **Despacho**: Invoca o handler C `syscall_handler(frame_addr)` em `src/kernel/kernel.c`.
4. **Restaurar Contexto**: Ao retornar, desempilha registradores, restaura o ponteiro de pilha do espaço de usuário, executa `swapgs` e roda `sysretq` para retornar ao Ring 3.

---

## 4. Tabela de Números de Syscall

O kernel trata as seguintes system calls em `syscall_handler()`:

| Número | Nome | Descrição |
| :---: | :--- | :--- |
| `1` | `SYS_WRITE` | Escreve dados em um descritor de arquivo |
| `2` | `SYS_OPEN` | Abre um arquivo ou nó de dispositivo |
| `3` | `SYS_READ` | Lê dados de um descritor de arquivo |
| `4` | `SYS_SPAWN` | Lança um binário de programa |
| `5` | `SYS_EXIT` | Encerra o processo ativo |
| `6` | `SYS_CREATE` | Cria um novo arquivo vazio |
| `7` | `SYS_WAIT` | Aguarda a conclusão de uma tarefa filha |
| `8` | `SYS_PIPE` | Estabelece um pipe IPC |
| `9` | `SYS_BRK` | Ajusta o limite de memória do heap do usuário |
| `10` | `SYS_SIGNAL` | Registra um handler de sinal customizado |
| `11` | `SYS_KILL` | Despacha um sinal para um processo |
| `12` | `SYS_SIGRETURN` | Completa o contexto de execução de sinal |
| `13` | `SYS_GETPROCS` | Recupera a lista de processos em execução |
| `14` | `SYS_DUP2` | Duplica um descritor de arquivo |
| `15` | `SYS_CLOSE` | Libera um descritor de arquivo aberto |
| `16` | `SYS_LIST` | Consulta o conteúdo de um diretório do sistema de arquivos |
| `17` | `SYS_SOCKET_SEND` | Envia dados por um socket |
| `18` | `SYS_SOCKET_RECV` | Recebe dados de um socket |
| `19` | `SYS_YIELD` | Cede voluntariamente o timeslice atual |
| `20` | `SYS_GET_TICKS` | Obtém a contagem de ticks do timer |
| `21` | `SYS_READDIR` | Lê entradas de diretório |
| `22` | `SYS_EXECVE` | Substitui a imagem do processo com novo ELF |
| `23` | `SYS_FORK` | Clona o espaço de endereçamento da tarefa atual |
| `24` | `SYS_SOCKET` | Cria um novo endpoint de socket |
| `25` | `SYS_BIND` | Associa um socket a um endereço/porta |
| `26` | `SYS_CONNECT` | Conecta um socket a um endereço remoto |
