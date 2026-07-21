# ELF Loader (Executable and Linkable Format)

Este documento especifica a implementação do carregador de binários ELF64 no kernel do PhotonOS (`src/kernel/elf.c`), abrangendo a validação de cabeçalhos, mapeamento de segmentos em memória, o trampolim de retorno de sinal e a otimização de compactação do linker.

---

## 1. Validação do Cabeçalho ELF64 (`elf_validate`)

Quando o kernel tenta executar um arquivo através de `sys_execve` ou `sys_spawn`:
1.  O arquivo é aberto via VFS e o cabeçalho inicial (`Elf64_Ehdr`) é lido em memória.
2.  A rotina `elf_validate` verifica a integridade e compatibilidade do arquivo com as seguintes especificações:
    *   **Número Mágico**: Os primeiros 4 bytes do campo `e_ident` devem ser `0x7F 'E' 'L' 'F'` (`ELFMAG`).
    *   **Classe**: Deve ser `ELFCLASS64` (`2`), indicando binário nativo de 64 bits.
    *   **Codificação**: Deve ser `ELFDATA2LSB` (`1`), indicando ordenação little-endian.
    *   **Tipo de Arquivo**: Deve ser `ET_EXEC` (`2`), indicando arquivo executável padrão.
    *   **Arquitetura**: Deve ser `EM_X86_64` (`62`), indicando AMD64/x86_64.
    *   **Geometria de Segmentos**: O tamanho do cabeçalho de programa (`e_phentsize`) deve corresponder ao tamanho de `Elf64_Phdr`.

---

## 2. Mapeamento de Segmentos em Memória (`map_segment`)

Uma vez validado o cabeçalho, o carregador lê a tabela de cabeçalhos de programa (Program Headers) para carregar os segmentos do tipo `PT_LOAD`:
1.  **Cálculo de Limites**: O endereço virtual do segmento é arredondado para baixo para o limite da página mais próxima via `align_down(p_vaddr)`, e o tamanho de memória total (`p_memsz`) é arredondado para cima via `align_up`.
2.  **Mapeamento de Permissões**: As flags de permissão do segmento ELF (`p_flags`) são mapeadas para flags de página do VMM:
    *   `PF_R` (Read) e `PF_X` (Execute) resultam em `PAGE_PRESENT | PAGE_USER`.
    *   `PF_W` (Write) adiciona a flag `PAGE_WRITABLE`.
3.  **Alocação Física e Cópia**:
    *   Para cada página do segmento, o kernel aloca um frame físico via `pmm_alloc()`.
    *   A página é rastreada na tabela do processo (`track_user_page`) para desalocação no `exit`.
    *   Se o offset do arquivo estiver contido no segmento (`p_filesz`), os dados correspondentes são lidos do disco via VFS e gravados no frame de memória recém-alocado. Qualquer espaço excedente (como a seção `.bss` em que `memsz > filesz`) é zerado pelo kernel.
    *   A página virtual é vinculada ao frame físico na PML4 do processo filho via `vmm_map_in_space`.

---

## 3. Configuração da Pilha e Início de Execução

Após carregar todos os segmentos:
*   **Pilha de Usuário**: O carregador aloca 4 páginas de pilha de usuário em alta memória terminando em:
    ```
    #define ELF_USER_STACK_TOP 0x0000008000010000ULL
    ```
    As páginas são mapeadas como presentes, graváveis e acessíveis pelo usuário.
*   **Trampolim de Sinais**: Uma página especial é mapeada em `0x0000008000008000` contendo o código de trampolim de retorno de sinal:
    ```nasm
    mov rax, 12   ; SYS_SIGRETURN
    syscall
    ```
    Este código é executado em Ring 3 para restaurar o contexto de registradores após o término de um handler de sinal.
*   **Ponto de Entrada**: O kernel carrega o valor `e_entry` do cabeçalho ELF no registrador RIP e o topo da pilha em RSP no contexto da tarefa antes de ceder o controle à CPU para iniciar a execução em Ring 3.

---

## 4. Otimização do Linker: Compactação `-N` (OMAGIC)

> [!TIP]
> **Eliminação de Padding de Alinhamento:**
> Por padrão, linkers alinham seções ELF a limites de 4 KiB (`0x1000`), inserindo milhares de bytes nulos em disco para preenchimento. Para mitigar o inchaço do binário final do kernel (que precisa caber nos limites do bootloader), os programas de usuário são linkados usando o parâmetro `-N` (OMAGIC).
>
> Isso desativa o alinhamento rígido de seções em disco e mescla as seções de código e dados em um único cabeçalho executável/gravável de carga contínua, economizando mais de 50 KiB de espaço no disco virtual de boot.
