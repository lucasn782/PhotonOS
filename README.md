# PhotonOS 🌌

O **PhotonOS** é um sistema operacional monolítico freestanding desenvolvido do zero para a arquitetura **x86_64**, operando em **64-bit Long Mode**. O projeto adota uma abordagem de **Arquitetura Limpa (Clean Architecture)**, implementando isolamento estrito de privilégios (Ring 0 vs Ring 3), gerenciamento avançado de memória, persistência nativa em sistemas de arquivos e um driver de rede emulado via barramento PCI.

---

## 🛠️ Arquitetura do Repositório

O projeto está estruturado de forma modular para garantir a separação de conceitos e builds estáveis:

```text
PhotonOS/
├── build/             # Todos os objetos (.o), binários (.elf) e mapa de memória
├── docs/              # Especificações de design, documentação e diagramas
├── include/           # Arquivos de cabeçalho (.h) do Kernel e drivers
├── logs/              # Telemetria de execução e relatórios do QEMU
├── scripts/           # Scripts automatizados de testes e montagem de disco
└── src/               # Código-fonte do sistema
    ├── boot/          # Código Assembly de baixo nível (Bootloader & Kernel Setup)
    ├── kernel/        # O núcleo do sistema (Memória, Escalonador, VFS, Syscalls)
    ├── drivers/       # Drivers de controle de hardware (Teclado V2, ATA, PCI, e1000)
    └── user/          # Biblioteca padrão (ulibc) e aplicações de espaço de usuário
```

## 🚀 Funcionalidades Consolidadas

- **Modo de Operação:** Transição de Modo Real de 16-bit para Modo Protegido de 32-bit, consolidando-se em 64-bit Long Mode com paginação em 4 níveis (PML4).
- **Gerenciamento de Memória:** Alocador de frames físicos (PMM) via Bitmap e alocador de Heap dinâmico (kmalloc/kfree) integrado.
- **Escalonamento Multitarefa:** Escalonador Round-Robin preemptivo baseado em interrupções periódicas do temporizador PIT (INT 0x20), com suporte a processos em segundo plano (`&`) e controle de jobs.
- **Espaço de Usuário (Ring 3):** Transição de privilégio protegida via TSS e chamadas de sistema eficientes via instruções nativas `syscall`/`sysret`.
- **Módulo de Entrada Modular V2.0:** Driver de teclado isolado com máquina de estados para suporte total ao layout ISO/Mac Português, rastreando o modificador AltGr para digitação estável do caractere Pipe (`|`).
- **Sistema de Arquivos FAT16 Persistente:** Driver ATA em modo PIO integrado ao Virtual File System (VFS), permitindo leitura e escrita dinâmica de blocos e metadados no disco secundário, com suporte a operadores de redirecionamento de fluxo (`>`).
- **Sinais Assíncronos:** Infraestrutura estável de tratamento de sinais baseada em Signal Trampoline para captura de interrupções via teclado (`Ctrl+C` -> `SIGINT`).
- **Rede Intel e1000:** Inicialização PCI/MMIO e rotinas TX/RX baseadas em ring buffers de descritores Ethernet no Ring 0.

## 📊 Status de Desenvolvimento do Núcleo

- **Conclusão de Engenharia Granular:** 95,4%
- **Macro-Etapas Concluídas:** 11 / 12

Atualmente, o sistema está na fase final de homologação do subsistema de rede, possuindo o mapeamento de barramento PCI e inicialização MMIO do controlador Intel e1000 totalmente funcionais em Ring 0.

## 🔧 Como Compilar e Executar

### Pré-requisitos (Ambiente WSL / Linux)

Certifique-se de ter instalado o toolchain de compilação cruzada (`gcc`, `ld`, `make`, `nasm`, `qemu`).

### 1. Compilar o Sistema

O processo de build utiliza compilação incremental isolada na pasta `build/` e gera automaticamente o mapa de símbolos hexadecimais `photon.map`:

```bash
make clean && make
```

### 2. Executar no QEMU (Via Linha de Comando)

Para inicializar o sistema com os dois discos rígidos virtuais (`photon.img` para boot e `disk.img` para armazenamento FAT16), interface e1000 e saída serial acoplada:

```bash
qemu-system-x86_64 -drive file=build/photon.img,format=raw,index=0,media=disk -drive file=build/disk.img,format=raw,index=1,media=disk -net nic,model=e1000 -serial stdio
```

Para conectar a interface e1000 ao NAT user-mode do QEMU, adicione `-net user` ao comando:

```bash
qemu-system-x86_64 -drive file=build/photon.img,format=raw,index=0,media=disk -drive file=build/disk.img,format=raw,index=1,media=disk -net nic,model=e1000 -net user -serial stdio
```

### 3. Executar no VirtualBox

Converta o disco rígido estruturado pelo Makefile para o formato nativo VDI:

```bash
qemu-img convert -f raw -O vdi build/disk.img build/disk.vdi
```

Crie uma VM configurada como `Other/Unknown (64-bit)`.

Remova a controladora SATA padrão e adicione uma Controladora IDE, conectando o `build/disk.vdi` como Master Primário, e uma Controladora Floppy, conectando o `build/photon.img`.

Ative o adaptador de rede em modo NAT sob o modelo Intel PRO/1000 MT Desktop (82540EM).
