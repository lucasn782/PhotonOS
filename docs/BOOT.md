# Arquitetura e Sequência de Boot (PhotonOS Boot Architecture)

Este documento descreve a arquitetura técnica completa do subsistema de inicialização (bootstrap) do **PhotonOS**, detalhando as etapas desde o firmware BIOS em Modo Real de 16 bits até o ponto de entrada do kernel em C (`kmain`) em Modo Longo de 64 bits.

---

## 1. Visão Geral do Pipeline de Boot

O processo de inicialização do PhotonOS segue um pipeline estrito em múltiplos estágios para transicionar o hardware de x86 Real Mode legado para x86_64 Long Mode nativo:

```text
       BIOS POST & MBR Search
                 │
                 ▼
     Boot Sector @ 0x0000:0x7C00 (16-bit Real Mode)
                 │
                 ├── Pilha inicial (SS:SP = 0x0000:0x7C00)
                 ├── Preserva DL (boot drive)
                 ├── Habilita linha de endereço A20 (INT 15h / Porta 0x92)
                 │
                 ▼
     Carregamento do Kernel via BIOS EDD LBA (INT 13h, AH=42h)
                 │
                 ├── Packet 1: 128 setores -> 0x0800:0x0000 (0x08000..0x17FFF)
                 ├── Packet 2: 128 setores -> 0x1800:0x0000 (0x18000..0x27FFF)
                 ├── Packet 3: 128 setores -> 0x2800:0x0000 (0x28000..0x37FFF)
                 └── Packet 4:  96 setores -> 0x3800:0x0000 (0x38000..0x43FFF)
                 │   (Fallback CHS se EDD não suportado)
                 ▼
     Salto para 0x0800:0x0000 (_start em src/boot/kernel.asm)
                 │
                 ├── Query & Configuração VBE VESA (Modo 0x4118, 1024x768x32 LFB)
                 ├── Carregamento da GDT temporária (lgdt)
                 ├── Ativação de CR0.PE (Bit 0 = 1)
                 │
                 ▼
     Modo Protegido de 32 bits (protected_start)
                 │
                 ├── Segmentos recarregados (DATA_SEL = 0x10, ESP = 0x7C00)
                 ├── Criação de Tabelas de Páginas de 4 níveis (PML4 @ 0x1000)
                 │   └── Identity Mapeamento de 128 MiB (0x00000000..0x08000000)
                 ├── Ativação de CR4.PAE (Bit 5 = 1)
                 ├── Carregamento de CR3 com PML4 (0x1000)
                 ├── Ativação de IA32_EFER.LME (Bit 8) e IA32_EFER.NXE (Bit 11)
                 ├── Ativação de CR0.PG (Bit 31), CR0.WP (Bit 16), CR0.PE (Bit 0)
                 │
                 ▼
     Salto Far Jump para 64-bit Long Mode (CODE64_SEL:long_mode_start)
                 │
                 ├── Registradores de segmento configurados (DS/ES/SS/FS/GS)
                 ├── Configuração de RSP (kernel_stack_top alinhado a 16 bytes)
                 │
                 ▼
     call kmain (src/kernel/kernel.c - Ring 0 C Entry Point)
```

---

## 2. Layout da Imagem de Disco (`photon.img`)

A imagem de boot gerada pelo `Makefile` consolida o setor de boot e o binário bruto do kernel:

```text
+-------------------------------------------------------------------------+
| LBA 0 (Setor 1)       | Boot Sector (512 bytes)                         |
|                       | Origem: src/boot/boot.asm -> build/boot/boot.bin|
|                       | Carregado pelo BIOS em 0x0000:0x7C00            |
+-------------------------------------------------------------------------+
| LBA 1 a 480           | Kernel Stage-2 Payload (480 setores = 240 KiB)  |
| (Setores 2 a 481)     | Origem: build/photon.bin                        |
|                       | Contém .text, .rodata, .data e blobs embutidos  |
|                       | Carregado na memória física 0x08000 a 0x43FFF   |
+-------------------------------------------------------------------------+
| LBA 481 a 2879        | Padding de Imagem Floppy 1.44M (1.474.560 bytes)|
+-------------------------------------------------------------------------+
```

### Constantes de Dimensionamento:
* `KERNEL_SECTORS = 480` (245.760 bytes = 240 KiB)
* `KERNEL_MAX_BYTES = KERNEL_SECTORS * 512 = 245.760 bytes`
* `FLOPPY_BYTES = 1.474.560 bytes` (1.44 MiB)

O `Makefile` valida automaticamente após o `objcopy` se o tamanho do binário gerado respeita o limite:
```makefile
$(KERNEL_BIN): $(KERNEL_ELF)
	$(OBJCOPY) -O binary $< $@
	test $$(stat -c%s $@) -le $(KERNEL_MAX_BYTES)
```

---

## 3. Carregamento do Kernel: Janelas LBA e Limite de 64 KiB

### 3.1 O Limite da Janela de 64 KiB no Modo Real
Em Modo Real (16 bits), os registradores de offset são limitados a 16 bits (`0x0000` a `0xFFFF`), abrangendo exatamente **64 KiB** por segmento. Além disso, muitos controladores de disco e rotinas de BIOS INT 13h possuem restrições de limite de DMA / Buffer Crossing em limites físicos de 64 KiB.

Se um pacote DAP (Disk Address Packet) solicitar mais de **128 setores** (128 * 512 = 65.536 bytes = 64 KiB), o offset de 16 bits no DAP (`KERNEL_OFF = 0x0000`) sofrerá *overflow* ou o BIOS rejeitará a operação retornando código de erro (`CF=1`).

### 3.2 As Quatro Janelas DAP (Disk Address Packets)
Para acomodar com segurança os 480 setores (240 KiB) do kernel, o `boot.asm` divide o carregamento LBA em **quatro janelas** de no máximo 128 setores cada, avançando o segmento de destino em `0x1000` (64 KiB físicos) a cada pacote:

| Pacote | Segmento:Offset | Endereço Físico | Setores | Tamanho | LBA Inicial | Finalidade |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| `disk_packet` | `0x0800:0x0000` | `0x08000`–`0x17FFF` | 128 | 64 KiB | LBA 1 (`0x01`) | Cabeçalho do kernel, entry `_start`, GDT, VBE code e `.text` inicial |
| `disk_packet2` | `0x1800:0x0000` | `0x18000`–`0x27FFF` | 128 | 64 KiB | LBA 129 (`0x81`) | Continuação do código executável `.text` e tabelas |
| `disk_packet3` | `0x2800:0x0000` | `0x28000`–`0x37FFF` | 128 | 64 KiB | LBA 257 (`0x101`) | Código `.text`, `.rodata` e drivers compilados |
| `disk_packet4` | `0x3800:0x0000` | `0x38000`–`0x43FFF` | 96 | 48 KiB | LBA 385 (`0x181`) | Fim da `.data`, blobs embutidos (`shell.elf`, initrd) |

**Total:** 128 + 128 + 128 + 96 = **480 setores** (240 KiB contíguos em `0x08000`–`0x43FFF`).

### 3.3 Estrutura do Disk Address Packet (DAP)
Cada DAP em `src/boot/boot.asm` segue a especificação EDD (Enhanced Disk Drive):
```nasm
align 4, db 0
disk_packet:
    db 0x10                     ; Tamanho do DAP = 16 bytes
    db 0x00                     ; Reservado (0)
    dw KERNEL_FIRST_SECTORS     ; Número de setores (128)
    dw KERNEL_OFF               ; Offset de destino (0x0000)
    dw KERNEL_SEG               ; Segmento de destino (0x0800)
    dq 0x0000000000000001       ; LBA inicial (1)
```

### 3.4 Fallback CHS e Diferença de Mídia
Caso o BIOS não suporte as extensões EDD (verificadas via `INT 13h, AH=41h, BX=55AAh`), o bootloader executa `load_kernel_chs`.
*   O fallback lê setor por setor (`INT 13h, AH=02h, AL=1`) incrementando o buffer `ES:BX` e alternando setores, cabeças e cilindros.
*   **Atenção de Arquitetura:** O algoritmo CHS utiliza a constante `SECTORS_TRACK = 18` (geometria padrão de disquete 1.44M). Em dispositivos de disco rígido (`DL=0x80`), essa geometria não reflete a geometria de discos rígidos BIOS, tornando o suporte LBA (`INT 13h, AH=42h`) o mecanismo primário e mandatório para mídias IDE/Hard Disk.

---

## 4. Transições de Modo de Operação da CPU

### 4.1 Modo Real (16 bits) -> Modo Protegido (32 bits)
Executado em `src/boot/kernel.asm`:
1.  **VBE Graphics Setup:** Consulta o VBE 2.0+ e ativa o modo `0x4118` (1024x768x32 Linear Framebuffer) via `INT 10h, AX=4F02h`. Salva os parâmetros em `boot_params`.
2.  **GDT (Global Descriptor Table):** Carrega a GDT contendo descritores nulo, código 64-bit (`0x08`), dados 64-bit/32-bit (`0x10`), código 32-bit (`0x18`), código Ring 3 (`0x20`), dados Ring 3 (`0x28`) e TSS (`0x30`).
3.  **Ativação do Modo Protegido:**
    ```nasm
    mov eax, cr0
    or eax, 0x00000001  ; Seta PE (Protection Enable)
    mov cr0, eax
    jmp dword CODE32_SEL:protected_start
    ```

### 4.2 Modo Protegido (32 bits) -> Modo Longo (64 bits)
Executado em `src/boot/kernel.asm` (`protected_start`):
1.  **Tabelas de Paginação (4 Níveis):**
    *   `pml4` alocada em endereço estático (`0x1000`).
    *   Cria mapeamento de identidade (*identity mapping*) cobrindo os primeiros **128 MiB** de memória física (`0x00000000` a `0x08000000`), garantindo que tanto o kernel (`0x8000`) quanto os buffers e estruturas de dados estejam acessíveis sem falhas de página.
2.  **Habilitação de Recursos da CPU:**
    *   `CR4.PAE` (Bit 5): Habilita extensão de endereço físico de 64 bits.
    *   `CR3`: Carrega a base física da `pml4` (`0x1000`).
    *   `IA32_EFER` (`0xC0000080`): Seta bit 8 (`LME` - Long Mode Enable) e bit 11 (`NXE` - No-Execute Enable).
    *   `CR0`: Seta bit 31 (`PG` - Paging), bit 16 (`WP` - Write Protect) e bit 0 (`PE`).
3.  **Far Jump para 64-bit:**
    ```nasm
    jmp CODE64_SEL:long_mode_start
    ```
4.  **Long Mode Entry:**
    *   Segmentos `DS`, `ES`, `SS`, `FS`, `GS` carregados com `DATA_SEL`.
    *   `RSP` inicializado em `kernel_stack_top` (alinhado a 16 bytes).
    *   `call kmain` desvia definitivamente para o código C do kernel.

---

## 5. Relação entre Tamanho do Kernel e Bootloader

O tamanho do kernel PhotonOS aumentou conforme novos subsistemas foram incorporados (TCP, VFS expandido, EXT2, Sinais POSIX, e1000, SMP).

*   Historicamente, `KERNEL_SECTORS` passou de 128 -> 256 -> 352 -> 416 -> **480 setores**.
*   **Regra de Ouro:** A constante `KERNEL_SECTORS` no `src/boot/boot.asm` e no `Makefile` deve **sempre** ser mantida igual ou superior ao tamanho real de `build/photon.bin`. Caso o binário do kernel cresça além de 480 setores (240 KiB), o quarto pacote DAP precisará ser expandido ou novos pacotes DAP deverão ser adicionados em segmentos subsequentes (`0x4800:0000`, etc.).

> [!TIP]
> **Oportunidade Arquitetural Futura:** O número de setores carregados deveria, idealmente, ser gravado no cabeçalho da imagem durante o processo de build (`build tool`) ou derivado automaticamente pelo bootloader lendo um cabeçalho fixo, eliminando a dependência de constantes manuais estáticas.
