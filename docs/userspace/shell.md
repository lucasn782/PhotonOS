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
*   **`exit`**: Termina a execução do shell.

### Execução de programas externos

Uma linha iniciada por caminho absoluto (por exemplo, `/bin/argprobe`) é dividida em argumentos. Aspas simples/duplas preservam espaços e `""` produz um argumento vazio; barra invertida escapa o próximo caractere. O shell faz `fork()`, chama `execve(path, argv, environ)` no filho e aguarda o filho em primeiro plano. Com `&`, registra o filho em background. O parser não expande variáveis nem procura programas em `PATH`. Consulte [execve_argv_env.md](execve_argv_env.md).

---

## 3. Pipeline de E/S (Redirecionamento `>`)

O shell intercepta o caractere `>` para redirecionar a saída padrão (`stdout`) de um comando para um arquivo físico no disco:
1.  **Divisão da String**: O caractere `>` é substituído por nulo (`\0`), dividindo a linha de comando em: (a) comando/parâmetros e (b) caminho do arquivo de destino.
2.  **Abertura do Arquivo**: O arquivo é criado/aberto via `SYS_CREATE` ou `SYS_OPEN` em modo de escrita, obtendo um descritor de arquivo (`fd`).
3.  **Duplicação de Descritor (`dup2`)**: O shell salva o descritor de stdout atual e usa a chamada de sistema `SYS_DUP2(fd, 1)` para redirecionar o stdout (descritor `1`) para o arquivo.
4.  **Execução**: O comando é executado com stdout modificado.
5.  **Restauração**: Após a execução, o stdout original é restaurado via `dup2` a partir de um descritor temporário salvo, e o arquivo é fechado.

---

## 4. Pipeline de E/S (Pipe `|`)

> [!IMPORTANT]
> **Pipe sequencial:**
> O shell conecta a saída do comando esquerdo à entrada do comando direito usando um pipe anônimo. O comando esquerdo é executado inline; filtros como `upper` e `rev` são iniciados pelo caminho de spawn existente.

O fluxo de processamento de um pipe (`|`) envolve:
1.  **Criação do Pipe**: O shell invoca `SYS_PIPE(pipefds)` (`8`), obtendo dois descritores: `pipefds[0]` (leitura) e `pipefds[1]` (escrita).
2.  **Saída do lado esquerdo**: duplica `stdout` para `pipefds[1]` e executa o comando esquerdo inline.
3.  **Entrada do lado direito**: restaura `stdout`, conecta `stdin` a `pipefds[0]` e executa o filtro ou comando direito.
4.  **Restauração**: fecha as pontas do pipe e restaura `stdin` e `stdout` originais.
