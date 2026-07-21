# Interactive Shell (/bin/shell)

Este documento especifica a implementação do shell interativo do PhotonOS (`src/user/shell.c`), incluindo os comandos internos, processamento de entrada e o pipeline de pipes e redirecionamento de E/S.

---

## 1. Loop de Execução Principal

O shell executa em espaço de usuário (Ring 3) e se comunica com o kernel exclusivamente via chamadas de sistema (syscalls). Ao iniciar, ele executa um loop de processamento contínuo:

```
[Exibe Prompt "PhotonOS />"] ──► [Lê Linha (via SYS_READ)]
                                         │
                                         ▼
                               [Tratamento de Sinais]
                                         │
                                         ▼
                               [Busca por '|' (Pipe)]
                                ├─ SIM ──► [Pipeline de Pipes]
                                └─ NÃO ──► [Busca por '>']
                                            ├─ SIM ──► [Redirecionamento]
                                            └─ NÃO ──► [Comando Simples]
```

O buffer de leitura de entrada captura teclas do teclado PS/2 até encontrar o caractere de nova linha (`\n`).

---

## 2. Comandos Internos (Built-in Commands)

O shell implementa nativamente as seguintes funcionalidades de conveniência:

*   **`help`**: Imprime a lista de comandos internos suportados e sua sintaxe.
*   **`clear`**: Limpa o terminal enviando sequências de escape ANSI compatíveis com o console dinâmico.
*   **`ls`**: Lista o conteúdo do diretório raiz ou caminhos utilizando a chamada de sistema `SYS_LIST` (`16`).
*   **`ps`**: Exibe a lista de processos ativos (PID, Nome, Estado, se está em Foreground ou Background) obtidos através da chamada de sistema `SYS_GETPROCS` (`13`).
*   **`cat [arquivo]`**: Abre um arquivo em modo somente leitura (`SYS_OPEN`) e imprime o seu conteúdo no console.
*   **`touch [arquivo]`**: Cria um arquivo vazio no disco através da chamada de sistema `SYS_CREATE` (`6`).
*   **`write [arquivo] [conteúdo]`**: Cria ou abre o arquivo e grava o texto fornecido.
*   **`yield`**: Abre mão voluntariamente do timeslice da CPU via `SYS_YIELD` (`19`).
*   **`fork`**: Testa a clonagem de processos executando um fork interno.
*   **`exec [programa]`**: Substitui a imagem do shell pelo novo programa ELF de usuário (`SYS_EXECVE`).
*   **`exit`**: Termina a execução do shell.

---

## 3. Pipeline de E/S (Redirecionamento `>`)

O shell intercepta o caractere `>` para redirecionar a saída padrão (`stdout`) de um comando para um arquivo físico no disco:
1.  **Divisão da String**: O caractere `>` é substituído por nulo (`\0`), dividindo a linha de comando em: (a) comando/parâmetros e (b) caminho do arquivo de destino.
2.  **Abertura do Arquivo**: O arquivo é criado/aberto via `SYS_CREATE` ou `SYS_OPEN` em modo de escrita, obtendo um descritor de arquivo (`fd`).
3.  **Duplicação de Descritor (`dup2`)**: O shell salva o descritor de stdout atual e usa a chamada de sistema `SYS_DUP2(fd, 1)` para redirecionar o stdout (descritor `1`) para o arquivo.
4.  **Execução**: O comando é executado com stdout modificado.
5.  **Restauração**: Após a execução, o stdout original é restaurado via `dup2` a partir de um descritor temporário salvo, e o arquivo é fechado.

---

## 4. Pipeline de Processos (Pipe `|`)

> [!IMPORTANT]
> **Pipeline de Processos Multitarefa:**
> O shell suporta a sintaxe `comando1 | comando2` para criar canais de comunicação IPC anônimos de fluxo unilateral entre dois processos Ring 3 independentes.

O fluxo de processamento de um pipe (`|`) envolve:
1.  **Criação do Pipe**: O shell invoca `SYS_PIPE(pipefds)` (`8`), obtendo dois descritores: `pipefds[0]` (leitura) e `pipefds[1]` (escrita).
2.  **Primeiro Fork (Comando Esquerdo)**:
    *   Bifurca o processo chamando `SYS_FORK`.
    *   No processo filho (esquerdo):
        *   Redireciona `stdout` (descritor `1`) para o lado de escrita do pipe (`pipefds[1]`) usando `SYS_DUP2`.
        *   Fecha as pontes não utilizadas do pipe.
        *   Invoca o comando esquerdo via `SYS_EXECVE`.
3.  **Segundo Fork (Comando Direito)**:
    *   Bifurca novamente o shell.
    *   No processo filho (direito):
        *   Redireciona `stdin` (descritor `0`) para o lado de leitura do pipe (`pipefds[0]`) usando `SYS_DUP2`.
        *   Fecha as pontes não utilizadas.
        *   Invoca o comando direito via `SYS_EXECVE`.
4.  **Sincronização no Pai**:
    *   O shell original (pai) fecha ambos os lados do pipe (`pipefds[0]` e `pipefds[1]`) em seu próprio espaço de endereçamento.
    *   Invoca `SYS_WAIT` sobre ambos os PIDs filhos para garantir que o prompt só retorne ao usuário após a conclusão de todo o pipeline.
