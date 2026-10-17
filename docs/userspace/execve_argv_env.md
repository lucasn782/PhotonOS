# ELF `argv`, `envp` e `execve()`

O syscall 22 implementa substituição da imagem do processo atual. A API pública da ulibc é `execve(path, argv, envp)`: em caso de erro retorna `-1` e mantém a imagem chamadora; em caso de sucesso não retorna à imagem anterior. O processo mantém PID, diretório atual e descritores abertos. O shell cria um filho com `fork()`, executa `execve()` nesse filho e aguarda com `wait()` ou deixa o filho em background.

## Entrada do programa

O carregador copia os vetores e suas strings do espaço do chamador antes de trocar CR3. A nova imagem recebe os registradores de entrada `RDI=argc`, `RSI=argv` e `RDX=envp`. A pilha começa com `argc`, seguido pelo vetor `argv`, seu ponteiro nulo, o vetor `envp` e seu ponteiro nulo; as strings apontadas ficam na mesma pilha. Na entrada C, `RSP % 16 == 8`, conforme a ABI System V AMD64. Não há vetor auxiliar (`auxv`) nesta ABI.

Programas que usam `main(argc, argv, envp)` devem ligar `src/user/crt0.c`. A rotina `_start` inicializa a variável global `environ`, chama `main()` e encerra com `exit()`. Programas legados com `_start` próprio continuam usando o contrato antigo de `SYS_SPAWN`; o shell encaminha comandos externos pelo novo caminho.

## Limites e validação

- Até 32 strings em cada vetor (`argv` e `envp`), além do terminador nulo.
- Até 255 bytes por string, sem contar o nulo final.
- Até 4096 bytes somados entre argumentos, ambiente e terminadores.
- A pilha inicial tem 4 páginas de 4096 bytes; o carregador verifica o espaço para strings e metadados antes de copiar.
- Entradas de ambiente precisam ter nome não vazio e o formato `NOME=VALOR`. Valores vazios e valores com `=` adicional são válidos.

O kernel valida caminho, vetores, cada ponteiro e cada string com os validadores de memória de usuário. Vetores sem nulo dentro do limite, ponteiros inválidos, strings longas, entradas de ambiente malformadas e ELF inválido são rejeitados. Os dados do chamador são copiados antes de carregar a imagem; nenhum ponteiro do espaço antigo é usado após a troca.

O ELF é preparado em um novo PML4. Cabeçalho, tabela de program headers, offsets, tamanhos de segmentos, endereços de usuário e entry point executável são verificados antes do commit. Segmentos não executáveis recebem NX. Falha durante preparação libera o espaço temporário e retorna `-1`. Após a preparação, o kernel atualiza o frame de syscall e o PML4 do processo, troca CR3 e só então destrói o espaço antigo. O status de erro segue a convenção atual da ulibc; não há `errno` nesta interface.

## Ambiente na ulibc

`environ` aponta para o `envp` recebido pelo programa e `getenv(name)` compara o nome completo até `=`. O ambiente é exatamente o vetor fornecido pelo chamador de `execve()`; não há herança automática no kernel. O chamador pode passar `environ` para herdar o próprio ambiente. `setenv()` e `unsetenv()` não fazem parte desta etapa.

## Testes

`src/user/argprobe.c` imprime argumentos e ambiente e verifica espaços, argumento vazio, valores vazios, `=` em valores e correspondência exata de `getenv()`. `src/user/execvetest.c` verifica falhas sem substituição, ponteiros e vetores inválidos, limites, ELF truncado, `fork()`/`execve()`/`waitpid()` e execuções repetidas. A suíte de integração QEMU é `scripts/test_execve_argv_env.py`.

O shell entende aspas simples e duplas e escapes com barra invertida ao montar argumentos de comandos externos. Não implementa expansão de variáveis, busca em `PATH` ou gramática POSIX completa. O caminho legado de `SYS_SPAWN` continua aceitando a string de argumento usada pelos programas atuais.
