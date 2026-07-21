# PhotonOS v4.1 🌌

O **PhotonOS v4.1** é um sistema operacional monolítico freestanding desenvolvido do zero para a arquitetura **x86_64**, executando em **64-bit Long Mode**. O projeto consolida um núcleo multi-core preemptivo robusto com Copy-On-Write (COW), descoberta dinâmica de CPUs via ACPI MADT, isolamento de falhas do Ring 3, pipeline gráfico por software, barramento PCI, interface de rede Intel e1000 estável, armazenamento persistente dual (FAT16 + EXT2), e uma biblioteca padrão de usuário (`ulibc`) otimizada com printf bufferizado.

---

## 🛠️ Estrutura do Projeto

```text
PhotonOS/
├── build/             # Binários gerados durante a compilação
├── docs/              # Documentação técnica dividida por subsistemas
│   ├── architecture/  # Core kernel, bootloader, scheduler, interrupts, apic
│   ├── memory/        # PMM, VMM, paging, heap, COW, TLB
│   ├── filesystem/    # VFS, FAT16, EXT2 nativo gravável
│   ├── networking/    # Stack de rede, e1000, ARP, IP, ICMP, UDP, TCP
│   ├── drivers/       # Framebuffer, keyboard, mouse, ATA, PCI
│   └── userspace/     # Shell, libc (ulibc), ELF loader, catalog de programas
├── include/           # Headers do Kernel e drivers
├── logs/              # Logs de execução e depuração serial
├── references/        # Materiais de referência e especificações
├── scripts/           # Scripts de montagem de imagem e automação
├── src/
│   ├── boot/          # Código de bootloader e entrada de kernel em assembly
│   ├── drivers/       # Drivers de rede, disco, entrada e periféricos
│   ├── fs/            # Implementações de sistemas de arquivos (EXT2)
│   ├── kernel/        # Código-fonte do núcleo e gerenciamento
│   └── user/          # Programas e utilitários Ring 3, ulibc
├── Makefile           # Script de compilação unificado do projeto
└── README.md          # Este arquivo de visão geral
```

---

## 🚀 Funcionalidades Consolidadas

### ⚡ Otimização da Biblioteca de Usuário (`ulibc`) — v4.1
*   **Buffered Printf (2048 bytes):** Implementação de um buffer local (`struct printf_buffer`) na ulibc. Em vez de disparar uma chamada de sistema `SYS_WRITE` por caractere, os dados são acumulados e descarregados em lotes (batching), reduzindo em até 2000 vezes as comutações Ring 3 ◄► Ring 0.
*   **Organização POSIX-like:** Segregação de APIs nos cabeçalhos padrão `<stdio.h>` e `<string.h>`.
*   **Interface Syscall Unificada:** Roteamento central das system calls de usuário baseadas na chamada inline `_syscall()` de 6 argumentos via registradores em conformidade com a convenção SysV ABI.

### 🛡️ Endurecimento do Kernel (Hardening) — v4.1
*   **Descoberta Dinâmica de CPUs via ACPI MADT:** Sondagem e leitura do RSDP no boot para localizar a tabela MADT do ACPI, coletando dinamicamente os APIC IDs de cores habilitados (com fallback automático para IDs 1, 2, 3).
*   **Isolamento de Falhas do Ring 3:** Exceções de Proteção Geral (#GP, Vetor 13) e Falha de Página (#PF, Vetor 14) originadas no espaço de usuário (`cs & 3 == 3`) encerram apenas a tarefa ofensora (`scheduler_exit_current(-1)`) em vez de causar KERNEL PANIC.
*   **Prevenção de Deadlocks no Idle Loop:** APs mantêm interrupções habilitadas (`sti; hlt`) no loop ocioso para permitir a entrega e confirmação imediata de IPIs de TLB Shootdown emitidas pelo BSP durante forks.

### 📦 Armazenamento Persistente Dual (FAT16 + EXT2) — v4.0
*   **EXT2 Nativo Gravável:** Superbloco, BGDT, aritmética modular de inodes, alocação de blocos por bitmaps, ponteiros diretos/indiretos e algoritmo de divisão de entradas de diretório.
*   **Blindagem Concorrente ATA (`ata_mutex`):** Exclusão mútua garantindo que apenas um núcleo acesse os registradores físicos do barramento IDE por vez.
*   **Detecção Automática e Fallback:** O VFS tenta montar `/dev/hda` como FAT16. Se a assinatura de boot falhar, executa automaticamente o fallback de montagem para o driver EXT2.

### 🧠 Copy-On-Write (COW) e Multiprocessamento (SMP) — v3.0 / v3.1
*   **Copy-On-Write:** Compartilhamento de frames físicos no `sys_fork` com separação de páginas preguiçosa via Page Fault, com controle de referências por frame físico (`pmm_refcounts`).
*   **TLB Shootdown Coordenado:** Emissão de IPIs via LAPIC (Vetor `0x79`) para forçar o flush de TLB em todos os outros processadores após alterações de mapeamentos de páginas de usuário.
*   **Bootstrap AP em Assembly:** Código trampolim alocado em `0x7000` transiciona processadores secundários de Modo Real (16-bit) para Long Mode (64-bit).

---

## 🔧 Compilação e Execução

### Dependências
*   GCC Cross Compiler (`x86_64-elf-gcc`)
*   Binutils (`x86_64-elf-ld`)
*   NASM (Netwide Assembler)
*   GNU Make
*   QEMU Emulator

### Compilar o Sistema
Para limpar o ambiente, compilar todos os arquivos do kernel e gerar a imagem de disco virtual com os binários de usuário:
```bash
make clean
make
make fat16-disk
```

### Executar no QEMU
**Modo Gráfico:**
```bash
qemu-system-x86_64 -drive format=raw,file=build/photon.img,if=floppy -drive format=raw,file=build/disk.img,if=ide,index=0,media=disk -netdev user,id=net0 -device e1000,netdev=net0
```

**Modo Headless (Console COM1 Serial no Terminal):**
```bash
qemu-system-x86_64 -drive format=raw,file=build/photon.img,if=floppy -drive format=raw,file=build/disk.img,if=ide,index=0,media=disk -netdev user,id=net0 -device e1000,netdev=net0 -nographic
```

---

## 📈 Roadmap Resumido

*   **Marcos Concluídos**: Inicialização 64-bit, alocador PMM/VMM, escalonador preemptivo Round-Robin, pipeline gráfico VBE com Double Buffering, driver de rede e1000, multiprocessamento SMP, Copy-On-Write (COW), sistema de arquivos EXT2 gravável, e printf bufferizado na ulibc.
*   **Em Desenvolvimento (v4.2-dev)**: Links simbólicos (symlinks) no VFS, transmissão de dados em sockets TCP stream, e sincronização concorrente na fila de caracteres do teclado em SMP.
*   **Planejado**: Permissões de arquivos Unix no VFS, montagem dinâmica de volumes adicionais, e endurecimento do espaço de endereçamento do kernel (NX/WP).

Para documentação extremamente detalhada de cada módulo, consulte o [Índice de Documentação Técnica](docs/DOCUMENTATION_INDEX.md).
