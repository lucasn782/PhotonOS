# Guia de Resolução de Problemas de Boot (Boot Troubleshooting Guide)

Este documento registra o diagnóstico, a análise de causa raiz, a correção de regressões de inicialização do **PhotonOS** e diretrizes técnicas para interpretar traces de emuladores (como o QEMU).

---

## 1. Regressão Histórica: Falha no Carregamento LBA do Kernel

### 1.1 Sintoma Observado
Durante a inicialização do PhotonOS em máquinas virtuais configuradas como disco rígido (ex: `-drive file=build/photon.img,format=raw,index=0,media=disk`), o boot travava logo após a mensagem do BIOS:
```text
Booting from Hard Disk...
```
A tela permanecia preta, a execução não avançava para o Modo Protegido e o console serial COM1 não exibia qualquer caractere de inicialização do kernel (`kmain`).

---

### 1.2 Diagnóstico e Cadeia Causal

```text
Bootloader BIOS (0x7C00)
       │
       ▼
load_kernel_lba (INT 13h, AH=42h)
       │
       ├── Packet 1 (128 setores @ 0x0800:0x0000) -> SUCESSO
       ├── Packet 2 (128 setores @ 0x1800:0x0000) -> SUCESSO
       └── Packet 3 (224 setores @ 0x2800:0x0000) -> FALHA (CF=1)
       │                                             (224 * 512 = 112 KiB > 64 KiB!)
       ▼
Salto para fallback load_kernel_chs
       │
       ├── Leitura CHS com geometria de disquete (SECTORS_TRACK = 18)
       ├── Incompatibilidade com unidade de disco rígido (DL = 0x80)
       └── Falha na leitura do primeiro setor (CF=1)
       │
       ▼
disk_error
       │
       ├── Escreve caractere 'E' (0x4F45) na memória de vídeo VGA (0xB8000)
       └── cli; hlt; jmp .halt (Trava de execução / Hang)
```

---

### 1.3 Causa Raiz

Com o crescimento do código-fonte do kernel (inclusão de drivers de rede e1000, TCP, VFS com symlinks/hardlinks, Sinais POSIX e ulibc), o `KERNEL_SECTORS` foi expandido para **480 setores** (240 KiB).

Na implementação anterior do bootloader, o carregamento havia sido dividido em apenas 3 pacotes DAP:
1. **Pacote 1:** 128 setores (64 KiB)
2. **Pacote 2:** 128 setores (64 KiB)
3. **Pacote 3:** 224 setores (114.688 bytes = 112 KiB)

**O Problema Arquitetural:**
*   Em Modo Real (16-bit), um registrador de segmento ou offset possui limite de 16 bits (`0xFFFF` = 64 KiB).
*   A chamada BIOS `INT 13h, AH=42h` (Extended Read) recebe no Disk Address Packet um ponteiro no formato `Segment:Offset` (`0x2800:0x0000`).
*   Transferir **224 setores** a partir do offset `0x0000` exige gravar até o offset `0x1C000`, o que excede a largura de 16 bits (`0xFFFF`), provocando *buffer boundary crossing* e rejeição imediata pelo BIOS com Carry Flag ativa (`CF=1`).

---

### 1.4 Efeito Secundário: Falha no Fallback CHS

Ao falhar no terceiro pacote LBA, o bootloader saltava para a rotina `load_kernel_chs`.
No entanto, o fallback CHS havia sido projetado com premissas de disquete legadas (`SECTORS_TRACK = 18`, 2 cabeças). Quando o emulador executa o PhotonOS a partir de um disco rígido (`DL=0x80`), a geometria CHS física/lógica do BIOS não corresponde a 18 setores por trilha. A primeira tentativa de leitura resultou em erro de disco, desviando para a rotina de pânico `disk_error`.

---

### 1.5 Correção Aplicada

A correção reestruturou o particionamento do carregamento LBA em `src/boot/boot.asm` em **quatro janelas** com limite estrito de **128 setores (64 KiB)** por operação:

```nasm
KERNEL_SECTORS        equ 480
KERNEL_FIRST_SECTORS  equ 128
KERNEL_SECOND_SECTORS equ 128
KERNEL_THIRD_SECTORS  equ 128
KERNEL_FOURTH_SECTORS equ KERNEL_SECTORS - KERNEL_FIRST_SECTORS - KERNEL_SECOND_SECTORS - KERNEL_THIRD_SECTORS ; 96
```

Configuração dos DAPs:
*   **Packet 1 (`disk_packet`):** 128 setores -> `0x0800:0x0000` (LBA 1..128)
*   **Packet 2 (`disk_packet2`):** 128 setores -> `0x1800:0x0000` (LBA 129..256)
*   **Packet 3 (`disk_packet3`):** 128 setores -> `0x2800:0x0000` (LBA 257..384)
*   **Packet 4 (`disk_packet4`):** 96 setores -> `0x3800:0x0000` (LBA 385..480)

Após a correção, a leitura dos 480 setores foi 100% concluída sem ultrapassar os limites de 64 KiB, permitindo o salto seguro para `0x0800:0x0000` e a inicialização bem-sucedida de `kmain`.

---

## 2. Diagnóstico com Logs de Trace do QEMU (`task_debug.log`)

Ao depurar problemas de boot com parâmetros avançados do QEMU, como:
```bash
qemu-system-x86_64 \
  -drive file=build/photon.img,format=raw,index=0,media=disk \
  -d int,cpu_reset,guest_errors \
  -D logs/task_debug.log
```

O arquivo de saída `logs/task_debug.log` conterá eventos emitidos pelo emulador em diferentes estágios do ciclo de vida da máquina virtual.

### 2.1 Registros de Firmware / SMM vs Exceções do Kernel

> [!IMPORTANT]
> **Aviso de Diagnóstico:** Registros de SMM (System Management Mode) e resets de CPU presentes no início do trace do QEMU são emitidos pelo firmware SeaBIOS durante a inicialização padrão da máquina virtual e **NÃO DEVEM** ser classificados automaticamente como Double Fault (#DF), Triple Fault ou exceção de software do PhotonOS.

Exemplo de registros normais emitidos pelo firmware/SeaBIOS:
```text
CPU Reset (CPU 0)
EAX=00000000 EBX=00000000 ECX=00000000 EDX=00060fb1
ESI=00000000 EDI=00000000 EBP=00000000 ESP=00000000
EIP=0000fff0 EFL=00000002 [-------] CPL=0 II=0 A20=1 SMM=0 HLT=0
ES =0000 00000000 0000ffff 00009300
CS =f000 ffff0000 0000ffff 00009b00
...
SMM: enter
CR3=00000000 EFER=0000000000000000
IDT=0000000000000000 000003ff
Servicing hardware INT=0x08
```

### 2.2 Como Diferenciar o Estado do Firmware do Estado do PhotonOS

Utilize os registradores da CPU no trace para determinar com precisão em qual estágio o evento ocorreu:

| Registrador | Firmware / SeaBIOS (Real Mode / SMM) | PhotonOS em Execução (64-bit Long Mode) |
| :--- | :--- | :--- |
| **CS** | `CS=f000` ou `CS=0000` (16-bit) | `CS=0008` (`CODE64_SEL`) ou `CS=0020` (Ring 3 User) |
| **CR0** | `CR0=60000010` (PE=0, PG=0) | `CR0=80010001` (PG=1, WP=1, PE=1) |
| **CR3** | `CR3=00000000` (sem paginação) | `CR3=00001000` (base PML4 do kernel) ou PML4 do processo |
| **CR4** | `CR4=00000000` (PAE desativado) | `CR4=00000020` (PAE ativo) |
| **EFER** | `EFER=0000000000000000` | `EFER=0000000000000900` (LME=1, NXE=1) |
| **IDTR** | `IDT=0000000000000000 000003ff` (IVT Real Mode) | `IDT=... limit=0x0FFF` (IDT 64-bit com 256 vetores) |
| **RIP / EIP** | `EIP <= 0x000FFFFF` | `RIP >= 0x00008000` ou endereços virtuais de kernel/usuário |

Se ocorrer um Triple Fault ou reset com `CS=0008` e `CR0=80010001`, a falha é do kernel PhotonOS. Se o evento ocorrer com `CS=f000` e `CR0=60000010`, trata-se de inicialização de firmware do BIOS.
