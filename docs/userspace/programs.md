# Programas de Usuário (Ring 3)

Este documento cataloga os programas de espaço de usuário distribuídos com o PhotonOS, descrevendo a função, a interface e os exemplos de uso de cada binário ELF64 disponível no disco de boot.

---

## 1. Visão Geral

Todos os programas de usuário residem em `src/user/` e são compilados como binários ELF64 estáticos linkados com a flag `-N` (OMAGIC) para compactação. Cada programa define seu ponto de entrada em `_start()` e comunica-se com o kernel exclusivamente via instruções `syscall`. Programas que usam funcionalidades avançadas (como `malloc`, `printf`, `signal`) linkam contra a `ulibc` (`src/user/ulibc.c`).

---

## 2. Catálogo de Programas

### 2.1. `shell` — Shell Interativo

*   **Arquivo**: `src/user/shell.c`
*   **Descrição**: Terminal interativo principal do PhotonOS. Gerencia entrada de usuário, interpreta comandos internos (`ls`, `ps`, `cat`, `touch`, `write`, `echo`, `clear`, `help`, `yield`, `fork`, `exec`, `exit`), processa pipelines (`|`) e redirecionamento de saída (`>`).
*   **Documentação Completa**: [shell.md](shell.md)

---

### 2.2. `ping` — Utilitário de Diagnóstico de Rede (ICMP)

*   **Arquivo**: `src/user/ping.c`
*   **Descrição**: Implementação Ring 3 do utilitário `ping`, permitindo diagnóstico de conectividade de rede via protocolo ICMP Echo Request/Reply.
*   **Fluxo de Execução**:
    1. Recebe o endereço IP de destino como argumento (`argv[1]`).
    2. Cria um socket raw (`socket(AF_INET, SOCK_RAW, IPPROTO_ICMP)`).
    3. Vincula o socket à porta local (`bind`).
    4. Conecta ao IP de destino (`connect`).
    5. Envia 4 pacotes ICMP Echo Request, cada um com 56 bytes de payload e identificador `0x5048`.
    6. Aguarda respostas com timeout baseado em ticks do kernel (`get_ticks()`).
    7. Imprime estatísticas de round-trip time (RTT) em milissegundos e taxa de perda.
*   **Exemplo de Uso**: `ping 10.0.2.2`

---

### 2.3. `hello` — Programa de Teste Mínimo

*   **Arquivo**: `src/user/hello.c`
*   **Descrição**: Programa de validação básica de Ring 3. Imprime a mensagem `"PhotonOS: Ola do espaco de usuario!"` via `SYS_WRITE` e termina com `SYS_EXIT(0)`.
*   **Propósito**: Testar o carregamento correto de ELF, a transição Ring 0 → Ring 3, a execução de syscalls e o retorno limpo ao scheduler.

---

### 2.4. `hang` — Teste de Sinais e SIGINT

*   **Arquivo**: `src/user/hang.c`
*   **Descrição**: Programa de teste de sinais. Registra um handler personalizado para `SIGINT` via `signal(SIGINT, on_sigint)` e entra em um loop infinito com `pause`.
*   **Comportamento**:
    *   **Primeiro Ctrl+C**: Intercepta o sinal, imprime `"Peguei o SIGINT, mas nao vou parar!"` e restaura o handler padrão (`signal(SIGINT, 0)`).
    *   **Segundo Ctrl+C**: Com o handler padrão restaurado, o sinal encerra o processo.
*   **Propósito**: Validar a entrega de sinais do kernel, o trampolim de retorno (`sigreturn`) e a restauração de handlers padrão.

---

### 2.5. `upper` — Filtro de Conversão para Maiúsculas

*   **Arquivo**: `src/user/upper.c`
*   **Descrição**: Lê até 128 bytes da entrada padrão (`stdin`, descritor `0`) via `SYS_READ`, converte cada caractere minúsculo ASCII (`a`–`z`) para maiúsculo (`A`–`Z`), e escreve o resultado em `stdout`.
*   **Propósito**: Demonstrar programas de filtro compatíveis com a sintaxe de pipes do shell (`echo "texto" | upper`).

---

### 2.6. `rev` — Filtro de Inversão de String

*   **Arquivo**: `src/user/rev.c`
*   **Descrição**: Lê até 256 bytes da entrada padrão, inverte a ordem dos caracteres usando alocação dinâmica (`malloc`/`free` da ulibc), e escreve o resultado invertido em `stdout`.
*   **Propósito**: Testar a alocação dinâmica de heap Ring 3 via `SYS_BRK` e a integração com pipelines.
*   **Exemplo de Uso**: `echo "photon" | rev` → saída: `notohp`

---

### 2.7. `spin` — Loop Infinito Mínimo

*   **Arquivo**: `src/user/spin.c`
*   **Descrição**: Loop infinito com instrução `pause`. Não faz chamadas de sistema.
*   **Propósito**: Testar a preempção forçada pelo timer IRQ 0 (PIT/APIC Timer), validando que o scheduler consegue preemptar processos que não cedem a CPU voluntariamente. Também usado para validar `Ctrl+C` (SIGINT) como mecanismo de término externo.

---

## 3. Compilação e Linkagem

Os programas de usuário são compilados pelo Makefile com as seguintes flags especiais:

```makefile
UFLAGS = -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
         -mno-red-zone -mcmodel=large
```

A linkagem utiliza o script `src/user/user.ld` e a flag `-N` (OMAGIC) para eliminar padding de alinhamento de seções. Os binários resultantes são copiados para o disco virtual de boot (FAT16 ou EXT2).
