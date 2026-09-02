# Processo de Inicialização (Boot Sequence)

Este documento descreve detalhadamente a sequência de inicialização do PhotonOS, desde o carregamento dos primeiros setores em Modo Real de 16 bits até a execução da rotina principal do kernel em C (`kmain`) em Modo Longo de 64 bits.

---

## 1. Estágio 1: Carregamento em Modo Real (src/boot/boot.asm)

No momento em que a máquina (física ou virtual) é ligada, o processador inicia em **Modo Real de 16 bits**, mapeando os primeiros endereços e buscando o setor de boot (Master Boot Record / Boot Sector) no dispositivo de boot selecionado. O BIOS carrega os 512 bytes deste setor no endereço físico fixo `0x0000:0x7C00` e desvia a execução para lá.

O código em `src/boot/boot.asm` executa as seguintes etapas:
1.  **Configuração da Pilha Inicial:** Define a pilha temporária em `SS:SP = 0x0000:0x7C00`, limpa as flags de direção (`cld`) e preserva o identificador do drive de boot BIOS em `DL`.
2.  **Habilitação da Linha A20:** Solicita habilitação da linha A20 via BIOS (`INT 15h, AX=2401h`) e confirmação através da porta de controle rápido `0x92` (Fast A20).
3.  **Carregamento do Kernel (Leitura LBA via BIOS EDD):** 
    *   Verifica suporte a extensões BIOS EDD via `INT 13h, AH=41h, BX=55AAh`.
    *   Utilizando a chamada `INT 13h, AH=42h` (Extended Read), o bootloader transfere os 480 setores do kernel compilado (`build/photon.bin`) divididos em **quatro pacotes DAP de até 128 setores** para respeitar o limite de 64 KiB do Modo Real:
        *   `disk_packet`: 128 setores carregados em `0x0800:0x0000` (Físico `0x08000`–`0x17FFF`).
        *   `disk_packet2`: 128 setores carregados em `0x1800:0x0000` (Físico `0x18000`–`0x27FFF`).
        *   `disk_packet3`: 128 setores carregados em `0x2800:0x0000` (Físico `0x28000`–`0x37FFF`).
        *   `disk_packet4`: 96 setores carregados em `0x3800:0x0000` (Físico `0x38000`–`0x43FFF`).
4.  **Fallback CHS:** Caso o BIOS não suporte EDD LBA, executa `load_kernel_chs` com leitura setor a setor (`INT 13h, AH=02h`).
5.  **Desvio para o Kernel:** Desvia a execução para `0x0800:0x0000` (físico `0x00008000`), ponto de entrada `_start` em `src/boot/kernel.asm`.

```text
BIOS POST ─► Boot Sector (0x7C00) ─► Habilita A20 ─► Carrega 480 Setores LBA (4 DAPs) ─► jmp 0800:0000
```

---

## 2. Estágio 2: Modo Real e Transição para Modo Protegido (src/boot/kernel.asm)

A execução inicia em `_start` ainda em Modo Real de 16 bits para interagir com os serviços de vídeo do BIOS:
1.  **Consulta e Configuração Gráfica (VBE/VESA):**
    *   Verifica a compatibilidade com a extensão VESA BIOS Extensions (VBE 2.0+) gravando `'VESA'` em `0x7E00` e chamando `INT 10h, AX=4F00h`.
    *   Consulta a informação do modo gráfico `1024x768` com 32 bits por pixel (`0x4118`) via `INT 10h, AX=4F01h`.
    *   Configura o modo de vídeo gráfico com Linear Framebuffer (LFB) ativo via `INT 10h, AX=4F02h` e salva os parâmetros (endereço físico LFB, resolução e pitch) na estrutura `boot_params`.
2.  **Carregamento da GDT:** Carrega a tabela de descritores globais (`lgdt [gdt_descriptor]`).
3.  **Transição para Modo Protegido de 32 bits:**
    *   Seta o bit 0 (PE - Protection Enable) no registrador de controle `CR0`.
    *   Executa um salto longo (`jmp dword CODE32_SEL:protected_start`), recarregando `CS` com o seletor `0x18` (código de 32 bits).

---

## 3. Estágio 3: Modo Protegido de 32 bits e Transição para Long Mode de 64 bits (src/boot/kernel.asm)

Operando em `protected_start` em Modo Protegido de 32 bits:
1.  **Recarga de Segmentos:** Carrega `DS`, `ES`, `FS`, `GS` e `SS` com `DATA_SEL` (`0x10`) e inicializa a pilha temporária em `ESP = 0x7C00`.
2.  **Construção das Tabelas de Páginas de Transição (Paging):**
    *   Zera a área de tabelas a partir de `0x1000` (`pml4`).
    *   Configura o encadeamento: `pml4[0]` -> `pdpt`, `pdpt[0]` -> `pd`, `pd[0..63]` -> `pts`.
    *   Mapeia por identidade (*identity map*) os primeiros **128 MiB** de memória física (`0x00000000` a `0x08000000`), permitindo a execução contínua sem falha de página.
3.  **Habilitação de Recursos de CPU de 64 bits:**
    *   Ativa a extensão de endereço físico (**PAE**) setando o bit 5 de `CR4`.
    *   Carrega a base da tabela PML4 (`0x1000`) no registrador `CR3`.
    *   Habilita o Modo Longo (**LME**) e a proteção de execução (**NXE**) no registrador MSR `IA32_EFER` (`0xC0000080`), setando os bits 8 e 11.
    *   Habilita a paginação e proteção de escrita setando os bits 31 (PG), 16 (WP) e 0 (PE) no registrador `CR0`.
4.  **Salto Far Jump para 64-bit Long Mode:**
    *   Executa `jmp CODE64_SEL:long_mode_start` usando o seletor `0x08`.

---

## 4. Estágio 4: Ponto de Entrada em Modo Longo de 64 bits (src/boot/kernel.asm)

A rotina `long_mode_start` executa em **Long Mode nativo de 64 bits** (Ring 0):
1.  **Recarga dos Registradores de Segmento:** Carrega `DS`, `ES`, `SS`, `FS` e `GS` com `DATA_SEL` (`0x10`).
2.  **Configuração da Pilha do Kernel:** Inicializa `RSP` com `kernel_stack_top` e garante alinhamento de 16 bytes (`and rsp, -16`).
3.  **Invocação do Kernel em C:** Executa a chamada `call kmain` para iniciar o pipeline de subsistemas do PhotonOS.

---

## 5. Estágio 5: Pipeline de Inicialização do Kernel C (src/kernel/kernel.c)

A função `kmain()` orquestra a inicialização sequencial dos componentes do sistema:

```text
kmain()
 ├── interrupts_disable()   (Garante cli no processador BSP)
 ├── serial_init()          (Configura UART COM1 a 115200 bps para logs klog)
 ├── idt_init() & idt_load()(Ativa e valida a IDT de 64 bits precocemente via sidt)
 ├── pmm_init()             (Inicializa o gerenciador de memória física via bitmap)
 ├── vmm_init()             (Inicializa PML4 do kernel, CR0.WP e EFER.NXE)
 ├── video_init()           (Mapeia LFB gráfico, aloca backbuffer e limpa tela)
 ├── vmm_self_test()        (Valida mapeamento e isolamento de páginas)
 ├── heap_init()            (Inicializa heap dinâmico do kernel com W^X e detecção UAF)
 ├── bcache_init()          (Inicializa buffer cache de blocos de disco)
 ├── ata_init()             (Detecta disco IDE primário em modo PIO e ata_mutex)
 ├── fat16_init()           (Monta partição FAT16 ativa em /dev/ata0p1 ou fallback)
 ├── pci_init()             (Varre barramento PCI buscando placas Ethernet e1000)
 ├── net_init()             (Configura buffers de rede, anéis DMA RX/TX, ARP, IP e sockets)
 ├── tcp_init()             (Inicializa subsistema de PCBs TCP e tabela global)
 ├── vfs_init()             (Inicializa o Virtual File System com nós de dispositivos)
 ├── apic_init()            (Desativa PIC 8259 legado e mapeia Local APIC MMIO)
 ├── tss_init()             (Registra Task State Segment para transição Ring 3)
 ├── syscall_init()         (Configura STAR, LSTAR e SFMASK para instruções syscall/sysret)
 ├── smp_init()             (Instala trampolim em 0x7000 e acorda APs secundários)
 ├── scheduler_init()       (Registra thread de rede e processo /bin/shell no escalonador)
 ├── keyboard_init()        (Configura buffers da IRQ 1 do teclado PS/2)
 ├── mouse_init()           (Configura controlador PS/2 8042 para mouse)
 ├── interrupts_enable()    (Habilita interrupções de hardware com sti)
 └── Loop 'hlt'             (O escalonador assume o controle via PIT timer IRQ 0)
```
