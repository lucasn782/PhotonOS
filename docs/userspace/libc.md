# Userspace Library (ulibc)

Este documento especifica a biblioteca padrão de espaço de usuário do PhotonOS (`ulibc`), descrevendo a interface unificada de system calls, o alocador dinâmico de memória (heap de usuário), funções de strings e a reescrita otimizada do printf bufferizado.

---

## 1. Interface Unificada de System Calls

A ulibc centraliza todas as comunicações com o kernel em Ring 0 utilizando a instrução inline `syscall` de 64 bits em assembly x86_64.

### Wrapper Geral de 6 Argumentos (`_syscall`)
Para unificar a passagem de parâmetros e garantir conformidade com a convenção de chamada AMD64 System V ABI, as system calls de usuário invocam a função estática centralizada:

```c
static inline long _syscall(long num, long a1, long a2, long a3, long a4, long a5, long a6) {
    long ret;
    register long r10 __asm__("r10") = a4;
    register long r8  __asm__("r8")  = a5;
    register long r9  __asm__("r9")  = a6;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"(num), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8), "r"(r9)
        : "rcx", "r11", "memory"
    );
    return ret;
}
```

Os wrappers individuais (como `syscall0` a `syscall4`) foram refatorados para atuar apenas como adaptadores sobre `_syscall`.

---

## 2. Alocador Dinâmico de Usuário (`malloc` / `free`)

A biblioteca fornece alocação dinâmica em Ring 3 baseada na chamada de sistema `SYS_BRK` (`9`) para mover o limite final de endereçamento do heap do processo.

### Estrutura de Metadados
Cada bloco de memória do heap é precedido por um cabeçalho de controle:
```c
struct malloc_block {
    size_t size;                // Tamanho utilizável pelo usuário
    int free;                   // Flag indicando se está livre (1) ou alocado (0)
    struct malloc_block *next;  // Ponteiro para o próximo bloco do heap
};
```

### Algoritmos
*   **`malloc(size)`**:
    1. Alinha o tamanho solicitado em blocos de 16 bytes (`align16`).
    2. Varre a lista a partir do cabeçalho `heap_head` procurando o primeiro bloco livre grande o suficiente (estratégia First-Fit).
    3. Se encontrar, faz a divisão do bloco (`split_block`) se sobrar espaço maior que o tamanho mínimo de um descritor, marcando o bloco alocado como ocupado.
    4. Se nenhum bloco livre servir, expande o heap (`extend_heap`) usando `SYS_BRK` em blocos múltiplos de tamanho de página (`4096` bytes).
*   **`free(ptr)`**:
    1. Obtém o cabeçalho recuando `sizeof(struct malloc_block)` a partir de `ptr`.
    2. Seta a flag `free = 1`.
    3. Percorre a lista inteira mesclando blocos contíguos livres (`coalesce_free_blocks`) para atenuar a fragmentação de memória.

---

## 3. Otimização: Printf Bufferizado

Nas versões anteriores à v4.1, a chamada `printf()` escrevia cada caractere de forma independente usando uma chamada `SYS_WRITE` por byte, gerando imenso overhead de comutação de contexto Ring 3 ◄► Ring 0.

A ulibc v4.1 introduz o **Buffered Printf**, agrupando as escritas na ulibc antes de despachar o bloco para o kernel:

```c
struct printf_buffer {
    char buf[2048];   // Buffer estático de 2 KiB
    int offset;       // Posição de escrita atual no buffer
    int written;      // Contador acumulado de bytes impressos
};
```

### Fluxo Operacional
1.  **Redirecionamento**: Caracteres gerados pela análise de formatação são passados para `printf_putc()`.
2.  **Acúmulo**: O caractere é salvo em `buf[offset]`.
3.  **Flush**: Quando o buffer atinge `2048` bytes (`offset >= PRINTF_BUF_SIZE`), ou ao final da rotina `printf()`, a função `printf_flush()` é acionada:
    *   Faz uma única chamada de sistema `SYS_WRITE` para o descritor `1` (stdout) com o tamanho do offset acumulado.
    *   Zera o offset para reuso.
4.  **Desempenho**: Reduz o tráfego de system calls em até 2000 vezes para strings longas.

---

## 4. Wrappers POSIX e API Disponibilizada

A ulibc fornece aos programas as seguintes APIs de sistema portáveis:

### Arquivos e E/S
*   `int open(const char *path, int flags)`
*   `int read(int fd, void *buf, int count)` (Note: Assinatura unificada POSIX com parâmetro `int count`)
*   `int write(int fd, const void *buf, int count)` (Note: Assinatura unificada POSIX com parâmetro `int count`)
*   `int close(int fd)`
*   `int readdir(int fd, vfs_dir_entry_t *buf, uint32_t count)`

### Processos e Controle
*   `int fork(void)`
*   `void exit(int status)`
*   `void yield(void)`
*   `uint64_t get_ticks(void)`
*   `int getprocs(proc_info_t *buffer, size_t max_size)`

### Sinais
*   `sighandler_t signal(int signum, sighandler_t handler)`
*   `int kill(int pid, int signum)`
*   `void sigreturn(void)` (Usada internamente no retorno do tratador de sinais)

### Sockets de Rede
*   `int socket(int domain, int type, int protocol)`
*   `int bind(int fd, const struct sockaddr *addr, uint32_t addrlen)`
*   `int connect(int fd, const struct sockaddr *addr, uint32_t addrlen)`
*   `uint32_t inet_addr(const char *ip_str)`
