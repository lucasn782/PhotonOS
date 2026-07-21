# Arquitetura do Núcleo do Sistema (PhotonOS)

Este documento detalha o design arquitetural de alto nível do PhotonOS, cobrindo o modelo de kernel monolítico, a segmentação de privilégios via Protection Rings do x86_64, a estrutura de tabelas de descritores de hardware (GDT, TSS e Interrupt Stack Tables), e a organização de diretórios do projeto.

---

## 1. Design de Sistema Monolítico

O PhotonOS é projetado como um **kernel monolítico** voltado para a arquitetura **x86_64** executando em **64-bit Long Mode (Ring 0)**. Todos os subsistemas fundamentais do sistema operacional — incluindo gerenciamento de memória física (PMM), gerenciamento de memória virtual (VMM), escalonador de tarefas, sistema de arquivos virtual (VFS), pilhas de rede e drivers de dispositivo (e1000, ATA, vídeo VBE) — executam no mesmo espaço de endereçamento linear com privilégio máximo de hardware.

Isso elimina o overhead de IPC (Inter-Process Communication) típico de micronúcleos, maximizando o desempenho de tráfego de rede e E/S de disco. A exclusão mútua e a segurança em ambientes multicore (SMP) são garantidas através de spinlocks e mutexes atômicos específicos por subsistema.

```text
┌─────────────────────────────────────────────────────────────┐
│                  Espaço de Usuário (Ring 3)                 │
│             Shell Interativo · Programas de Usuário         │
├──────────────────────────────┬──────────────────────────────┤
│            Interface de Syscalls (syscall / sysret)         │
├──────────────────────────────┴──────────────────────────────┤
│                   Espaço de Kernel (Ring 0)                 │
│                                                             │
│  ┌───────────────────────┐  ┌────────────────────────────┐  │
│  │      Escalonador      │  │      Memória (VMM/PMM)     │  │
│  └───────────┬───────────┘  └─────────────┬──────────────┘  │
│              │                            │                 │
│  ┌───────────▼───────────┐  ┌─────────────▼──────────────┐  │
│  │   VFS / Filesystems   │  │   Drivers de Dispositivo   │  │
│  └───────────────────────┘  └────────────────────────────┘  │
└─────────────────────────────────────────────────────────────┘
```

---

## 2. Segmentação de Privilégios (Protection Rings)

O isolamento entre o espaço do kernel e o espaço de usuário é rigidamente imposto no nível de hardware utilizando os anéis de proteção do processador x86:

*   **Ring 0 (Supervisor Mode):** Onde reside e executa todo o código do Kernel. O kernel tem acesso irrestrito a toda a memória física mapeada, portas de E/S de hardware (através de instruções `in`/`out`), MSRs (Model Specific Registers) e registradores de controle da CPU (`CR0`, `CR2`, `CR3`, `CR4`). Instruções privilegiadas (como `hlt`, `cli`, `sti`, `invlpg`, `lgdt`, `lidt`) podem ser executadas diretamente.
*   **Ring 3 (User Mode):** Onde rodam as aplicações de espaço de usuário (como o shell `/bin/shell` e programas de utilidades). O acesso à memória é stritamente limitado ao espaço de endereçamento virtual mapeado com a flag `PAGE_USER`. Aplicações Ring 3 não podem acessar portas de E/S diretamente nem executar instruções privilegiadas. Qualquer violação de privilégio resulta em uma exceção de Proteção Geral (`#GP`, Vetor 13) ou Falha de Página (`#PF`, Vetor 14).

---

## 3. Estruturas de Descritores Globais e TSS

A transição estável para o Modo Longo de 64 bits e a segurança de execução do Ring 0 exigem a correta configuração da **GDT (Global Descriptor Table)** e do **TSS (Task State Segment)**.

### A. Alinhamento da GDT em 64 bits
No modo de 64 bits, o registrador GDTR necessita de um limite de 16 bits seguido de um endereço de base linear completo de 64 bits. No PhotonOS, o descritor de GDT é estruturado de forma a evitar qualquer truncamento de endereço de alta memória (*higher-half*):

```assembly
gdt_descriptor:
    dw gdt_end - gdt_start - 1  ; Limite da tabela (16 bits)
    dq gdt_start                ; Endereço Base de 64 bits (quadword)
```

Os seletores de segmento configurados na GDT abrangem:
*   `0x08`: Kernel Code Segment (base=0, limit=0, flags de 64-bit e Ring 0)
*   `0x10`: Kernel Data Segment
*   `0x18`: Compatibility Mode 32-bit Code Segment
*   `0x20` (ou `0x23` com RPL=3): User Data Segment (Ring 3)
*   `0x28` (ou `0x2B` com RPL=3): User Code Segment (Ring 3)
*   `0x30`: Segmento de TSS (Task State Segment Descriptor - estendido para 16 bytes em 64 bits, ocupando offsets `0x30` e `0x38`)

### B. TSS e Prevenção de Falhas Triplas (IST1)
Quando ocorre uma exceção grave no Ring 0 (como estouro de pilha do kernel), a pilha ativa do processador (`rsp`) torna-se inválida. Se o processador tentar empilhar o frame de interrupção nessa mesma pilha corrompida, ocorre uma falha secundária. A incapacidade de processar essa falha secundária faz o hardware disparar uma Falha Tripla (Triple Fault), reiniciando a CPU imediatamente.

Para mitigar isso, o PhotonOS implementa a tabela de pilhas de interrupção (**IST - Interrupt Stack Table**) contida na TSS:
1.  **Pilha Dedicada de Double Fault:** É alocada uma página de pilha estática isolada no kernel para tratar falhas de página graves e double faults:
    ```c
    static uint8_t double_fault_stack[4096] __attribute__((aligned(4096)));
    ```
2.  **Configuração do IST1:** O campo `ist1` da estrutura `tss64` (`kernel_tss`) é configurado para apontar para o topo da pilha isolada (`double_fault_stack + 4096`).
3.  **Mapeamento na IDT:** O descritor da interrupção de Double Fault (`idt[8]`) é explicitamente configurado com o índice IST correspondente:
    ```c
    idt[8].ist = 1; // Associa a interrupção ao IST1
    ```
Dessa forma, caso ocorra um estouro de pilha no Ring 0, o hardware comuta automaticamente para a `double_fault_stack` ao invocar a `double_fault_handler`, permitindo que o kernel registre o diagnóstico no console serial via `klog` e pare o processador de forma controlada em vez de resetar a máquina.

---

## 4. Estrutura de Diretórios do Projeto

O PhotonOS é organizado de forma modular, dividindo as responsabilidades de compilação da seguinte forma:

*   **`src/boot/`**: Contém o bootloader em linguagem assembly. Inclui `boot.asm` (carregador do setor de boot em Modo Real) e `kernel.asm` (trampolim, configuração inicial de paginação e GDT de 32 para 64 bits).
*   **`src/kernel/`**: O coração do kernel. Implementa o gerenciamento de memória (virtual, física, heap), escalonador, tratamento de interrupções (APIC, IDT), rede (e1000, ARP, IP, UDP, TCP) e o loader ELF.
*   **`src/drivers/`**: Abstrações de hardware. Contém drivers do mouse PS/2, teclado, vídeo VBE (Linear Framebuffer), barramento PCI, interface de disco ATA e interface de rede e1000.
*   **`src/fs/`**: Implementações de sistemas de arquivos, como o driver do EXT2 gravável e do FAT16 gravável.
*   **`src/user/`**: Código-fonte dos programas executados em espaço de usuário (Ring 3), incluindo a biblioteca padrão de usuário (`ulibc`) e o console shell interativo.
*   **`include/`**: Arquivos de cabeçalho (.h) globais mapeando estruturas e protótipos de funções do kernel e da ulibc.
