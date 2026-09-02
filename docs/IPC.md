# PhotonOS v4.3 — Arquitetura de IPC e Pipes

## 1. Visão Geral

O subsistema de Comunicação Inter-Processos (IPC) do PhotonOS v4.3 baseia-se em canais unidirecionais de fluxo de bytes (*anonymous pipes*) integrados à Virtual File System (VFS), gerenciamento de descritores de arquivos e sinais POSIX.

---

## 2. Estrutura de Dados do Pipe

```c
#define PIPE_BUFFER_SIZE 4096

struct pipe_buffer {
    uint8_t data[PIPE_BUFFER_SIZE];  /* Buffer circular */
    size_t read_pos;                 /* Offset de leitura */
    size_t write_pos;                /* Offset de escrita */
    size_t count;                    /* Bytes presentes no buffer */
    uint32_t readers;                /* Contador de descritores de leitura ativos */
    uint32_t writers;                /* Contador de descritores de escrita ativos */
    mutex_t lock;                    /* Mutex de sincronização */
};

struct pipe_end {
    struct pipe_buffer *pipe;        /* Ponteiro para o buffer compartilhado */
    int readable;                    /* Flag indicando ponta de leitura */
    int writable;                    /* Flag indicando ponta de escrita */
};
```

---

## 3. Semântica de Leitura, Escrita e Fechamento

### 3.1. Escrita (`pipe_write`)
1. **Broken Pipe**: Se `pipe->readers == 0`, a escrita emite `SIGPIPE` para o processo escritor e retorna `-1`.
2. **Buffer Cheio**: Se `pipe->count == PIPE_BUFFER_SIZE`, o escritor dorme com motivo `TASK_WAIT_PIPE_WRITE`.
3. **Sucesso**: Escreve até a capacidade disponível, acorda leitores via `scheduler_wake_pipe_readers()` e retorna o número de bytes gravados.

### 3.2. Leitura (`pipe_read`)
1. **Dados Disponíveis**: Transfere do buffer circular até o tamanho solicitado.
2. **Buffer Vazio & Escritores Fechados (`pipe->writers == 0`)**: Retorna `0` imediatamente, sinalizando **End-of-File (EOF)** ao leitor.
3. **Buffer Vazio & Escritores Ativos**: O leitor dorme com motivo `TASK_WAIT_PIPE_READ`.

### 3.3. Fechamento de Pontas (`pipe_node_close`)
Quando um descritor de pipe é fechado:
1. Sob o mutex do pipe, decrementa `readers` (se era ponta de leitura) ou `writers` (se era ponta de escrita).
2. Se `readers == 0`, acorda escritores bloqueados para que detectem Broken Pipe.
3. Se `writers == 0`, acorda leitores bloqueados para que detectem EOF (`0`).
4. Se `readers == 0 && writers == 0`, o buffer do pipe é desalocado com `kfree(pipe)`.

---

## 4. Compartilhamento entre Processos

1. **`sys_pipe(int fds[2])`**: Aloca dois descritores de arquivo associados a nós VFS `VFS_NODE_PIPE` referenciando a mesma estrutura `struct pipe_buffer` com `readers = 1` e `writers = 1`.
2. **`fork()`**: O processo filho clona os descritores do pai, incrementando o `ref_count` de cada `file_description_t`.
3. **Pipelines de Shell** (`cmd1 | cmd2`): O shell redireciona o `stdout` (fd 1) de `cmd1` para a ponta de escrita do pipe (`fds[1]`) e o `stdin` (fd 0) de `cmd2` para a ponta de leitura (`fds[0]`), fechando as pontas não utilizadas em cada processo.
