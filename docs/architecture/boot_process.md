# Processo de Inicialização (Boot Sequence)

Este documento descreve detalhadamente a sequência de inicialização do PhotonOS, desde o carregamento dos primeiros setores em Modo Real de 16 bits até a execução da rotina principal do kernel em C em Modo Longo de 64 bits.

---

## 1. Estágio 1: Carregamento em Modo Real (src/boot/boot.asm)

No momento em que a máquina (física ou virtual) é ligada, o processador inicia em **Modo Real de 16 bits**, mapeando os primeiros endereços e buscando o setor de boot (Master Boot Record / Boot Sector) no dispositivo de boot selecionado. O BIOS carrega os 512 bytes deste setor no endereço físico fixo `0x7C00` e desvia a execução para lá.

O código em `src/boot/boot.asm` executa as seguintes etapas:
1.  **Configuração da Pilha Inicial:** Define a pilha temporária em `0x7C00` e limpa as flags de direção.
2.  **Carregamento do Kernel (Leitura via BIOS):** Utilizando a chamada de interrupção de disco `INT 0x13, AH=0x02` (ou extensões LBA se disponível), o bootloader lê os setores subsequentes do disco (que contêm o kernel principal compilado) e os escreve na memória RAM a partir do endereço físico `0x8000`.
3.  **Consulta e Configuração Gráfica (VBE/VESA):**
    *   Verifica a compatibilidade com a extensão VESA BIOS Extensions (VBE 2.0+).
    *   Consulta a informação do modo gráfico `1024x768` com profundidade de cor de 32 bits por pixel (Modo VBE `0x4118`).
    *   Se disponível, configura a placa de vídeo para esse modo usando `INT 0x10, AX=0x4F02` e lê e salva os parâmetros do Linear Framebuffer (LFB), como o endereço base físico da memória de vídeo, largura, altura e bytes por linha gráfica, salvando-os em uma estrutura temporária (`boot_params`).
    *   Caso falhe, imprime um erro na memória de texto VGA legada (`0xB8000`) e interrompe o boot.

```text
BIOS Post ─► Carrega Setor de Boot (0x7C00) ─► Lê Kernel (0x8000) ─► Configura VBE VESA Mode ─► Salva LFB Params
```

---

## 2. Estágio 2: Modo Protegido de 32 bits (src/boot/kernel.asm)

Ainda no arquivo `src/boot/boot.asm`, é realizada a transição inicial de Modo Real (16 bits) para Modo Protegido (32 bits):
*   Desativa interrupções de hardware (`cli`).
*   Configura uma GDT temporária de 32 bits.
*   Seta o bit 0 (PE - Protection Enable) no registrador de controle `CR0`.
*   Executa um salto longo (`jmp`) para recarregar o registrador `CS` com o seletor de Modo Protegido de 32 bits (`CODE32_SEL = 0x08`), desviando o fluxo para `_start` em `src/boot/kernel.asm`.

Operando em **Modo Protegido de 32 bits**, o código prepara a CPU para a transição definitiva para o **64-bit Long Mode**:
1.  **Construção das Tabelas de Páginas de Transição (Paging):**
    *   Cria tabelas de paginação de 4 níveis temporárias na RAM (geralmente iniciando em `0x1000`).
    *   Configura um mapeamento de identidade (*identity map*) para os primeiros 2 GiB de memória física. Isso é essencial para que o processador não gere uma falha de página imediata assim que a paginação for ativada (já que o RIP corrente está operando em endereços físicos baixos, ex. `0x8000+`).
    *   Mapeia o kernel na alta memória (*higher-half*) em `0xFFFFFFFF80000000` apontando para o endereço físico `0x00000000` (deslocado).
2.  **Habilitação das Extensões de Hardware:**
    *   Ativa a extensão de endereço físico (**PAE - Physical Address Extension**) setando o bit 5 de `CR4`.
    *   Carrega a base da tabela PML4 temporária (endereço `0x1000`) no registrador `CR3`.
    *   Habilita o Modo Longo (**LME - Long Mode Enable**) no registrador MSR de controle `IA32_EFER` (`0xC0000080`), setando o bit 8.
    *   Habilita a paginação setando simultaneamente o bit 31 (PG - Paging) e o bit 0 (PE) no registrador `CR0`.
3.  **Salto Longo para Modo Longo de 64 bits:**
    *   Realiza um salto longo far jump usando o descritor de segmento de código de 64 bits (`CODE64_SEL`) para a etiqueta `kernel_entry` de 64 bits.

---

## 3. Estágio 3: Ponto de Entrada em Modo Longo de 64 bits (src/boot/kernel.asm)

A etiqueta `kernel_entry` é executada em **Long Mode de 64 bits** (privilégio Ring 0):
1.  **Recarga dos Registradores de Segmento:** Carrega `DS`, `ES`, `SS`, `FS` e `GS` com o seletor de dados de kernel de 64 bits (`0x10`).
2.  **Configuração da Pilha do Kernel:** Inicializa o ponteiro de pilha `RSP` apontando para uma área reservada de pilha de bootstrap (`stack_top`).
3.  **Ativação do Coprocessador Matemático (FPU/SSE):**
    *   Manipula os registradores `CR0` e `CR4` para habilitar a execução de instruções SSE e desativar emulação de FPU.
    *   Garante que operações matemáticas rápidas em Ring 0 e Ring 3 possam executar sem causar interrupções de exceção de coprocessador não disponível.
4.  **Invocação do Kernel em C:** Executa a chamada `call kmain` para desviar a execução definitivamente para o código C em `src/kernel/kernel.c`.

---

## 4. Estágio 4: Pipeline de Inicialização do Kernel C (src/kernel/kernel.c)

A função `kmain()` em C é o ponto central onde todos os drivers e subsistemas do PhotonOS são orquestrados e inicializados de forma sequencial:

```text
kmain()
 ├── interrupts_disable()   (Desativa todas as interrupções de hardware locais - cli)
 ├── serial_init()          (Configura a porta serial COM1 a 115200 bps para logs de klog)
 ├── pmm_init()             (Inicializa o gerenciador de memória física via bitmap)
 ├── vmm_init()             (Inicializa estruturas de tabelas de paginação PML4)
 ├── video_init()           (Limpa e mapeia o Linear Framebuffer gráfico e configura backbuffer)
 ├── vmm_self_test()        (Valida leitura/escrita em páginas mapeadas para testes)
 ├── heap_init()            (Inicializa o alocador dinâmico de kernel - kmalloc / kfree)
 ├── vfs_init()             (Inicializa o Virtual File System)
 ├── initrd_init()          (Inicializa e monta o Ramdisk de boot em memória)
 ├── ata_init()             (Detecta disco IDE rígido e inicializa mutex de concorrência)
 │    └── ata_vfs_init()    (Monta o sistema de arquivos FAT16 ou faz fallback para EXT2)
 ├── pci_init()             (Varre barramento PCI buscando placas Ethernet e1000)
 │    └── net_init()        (Configura buffers de rede, anéis de DMA RX/TX, ARP, IP e sockets)
 ├── console_nodes_init()   (Mapeia stdin, stdout e stderr no VFS para Ring 3)
 ├── apic_init()            (Desativa PIC 8259 legado e mapeia o Local APIC)
 ├── smp_init()             (Configura código trampolim em 0x7000 e inicia os cores APs secundários)
 ├── tss_init()             (Registra a TSS para Ring 3)
 ├── syscall_init()         (Configura registradores STAR, LSTAR e SFMASK para syscalls)
 ├── scheduler_init()       (Registra a thread de rede e o processo shell no escalonador)
 ├── keyboard_init()        (Instala a fila de buffers do driver de teclado)
 ├── mouse_init()           (Configura a porta de mouse PS/2 8042 e habilita streaming de pacotes)
 ├── interrupts_enable()    (Habilita interrupções no processador BSP - sti)
 └── Execução indefinida: Loop 'hlt' (O escalonador assume o controle via PIT timer IRQ 0)
```
Ao final do pipeline de `kmain`, as interrupções são ligadas. O temporizador PIT (ou LAPIC timer) gera a primeira interrupção periódica, disparando a rotina do escalonador que realiza o desvio de privilégio e carrega o primeiro processo registrado, `/bin/shell`, em espaço de usuário (Ring 3).
