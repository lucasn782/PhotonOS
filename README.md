# PhotonOS v4.2 🚀

O **PhotonOS v4.2** é um sistema operacional monolítico freestanding desenvolvido do zero para a arquitetura **x86_64**, executando em **64-bit Long Mode**. O projeto consolida um núcleo multi-core preemptivo robusto com Copy-On-Write (COW), descoberta dinâmica de CPUs via ACPI MADT, isolamento de falhas do Ring 3, pipeline gráfico por software, barramento PCI, interface de rede Intel e1000 estável, armazenamento persistente dual (FAT16 + EXT2), biblioteca padrão de usuário (`ulibc`) otimizada com printf bufferizado e uma nova infraestrutura completa de protocolo TCP (Fase 1).

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

### 🛡️ Boot Recovery & Estabilização (v4.2 — Boot Recovery)
*   **Inicialização Antecipada da IDT:** A IDT é ativada e validada via `sidt` logo na entrada de `kmain()`, eliminando Triple Faults e garantindo relatórios explicativos de Kernel Panic no COM1.
*   **Relocação da Seção `.bss` (0x00100000):** A `.bss` do kernel foi realocada para 1 MiB (`0x100000`), eliminando o conflito com a memória VGA (`0xA0000`–`0xBFFFF`) e restaurando a integridade dos descritores do PMM.
*   **Heap Seguro:** `heap_expand()` valida explicitamente os retornos de `pmm_alloc()` e `vmm_map()`, abortando o crescimento do heap em caso de falha sem tentar gravar em memória não mapeada.
*   **Validação Estrita de MMIO:** O VMM aceita mapeamentos de regiões de hardware MMIO (VBE LFB em `0xFD000000` e APIC em `0xFEE00000`) sem expurgar entradas de tabelas de páginas.

### 🌐 Infraestrutura TCP (Fase 1 — v4.2)
*   **Subsistema TCP Modular:** Módulo kernel independente (`src/kernel/tcp.c`, `include/tcp.h`) contendo gerenciamento de PCBs em lista global protegida por mutex, cálculo de checksum RFC 793 com pseudo-cabeçalho IPv4 e demultiplexação pela tupla de 4 elementos ou socket listener.
*   **Gerenciamento de Portas:** Algoritmo thread-safe de alocação de portas efêmeras IANA (49152–65535), `bind` explícito de portas locais, detecção de colisão e liberação segura sem vazamentos.
*   **Sockets e VFS:** Suporte nativo a `socket(AF_INET, SOCK_STREAM)`, `bind`, `connect`, `listen`, `accept`, `read` e `write` integrados aos nós do VFS e ao escalonador preemptivo sem espera ocupada ("zero busy-wait").
*   **Estados do TCP:** Suporte completo aos 5 estados da Fase 1 (`CLOSED`, `LISTEN`, `SYN_SENT`, `SYN_RECEIVED`, `ESTABLISHED`) com filas de segmentos dinâmicos e suporte a temporizadores de retransmissão (RTO), Keep-Alive e Delayed ACK.

### 📂 Infraestrutura do VFS e Mount Manager (Sprint 3) — v4.3-fs
*   **Permissões POSIX Simplificadas (mode_t):** Controle de acesso octal (`0755`/`0644`), validação `vfs_check_permission()` e chamadas `chmod()` / `chown()`.
*   **Gestão de Identidade (UID/GID):** Herança de `uid` e `gid` por processo em tarefas do escalonador.
*   **Hard Links (`link`/`unlink`):** Criação de nós compartilhados no VFS e controle de referências físicas `nlink` com desalocação em `nlink == 0`.
*   **Symbolic Links (`symlink`/`readlink`):** Suporte a nós `VFS_NODE_SYMLINK` com resolução recursiva `vfs_find_following_symlinks()` limitada a 8 níveis.
*   **Mount Manager & Tabela Global de Mounts:** Gerenciamento de múltiplos volumes via `vfs_mount_list`, `vfs_mount` e `vfs_umount` com redirecionamento transparente de caminhos `mounted_here`.

### ⚡ Otimização da Biblioteca de Usuário (`ulibc`) — v4.1
*   **Buffered Printf (2048 bytes):** Implementação de um buffer local (`struct printf_buffer`) na ulibc. Em vez de disparar uma chamada de sistema `SYS_WRITE` por caractere, os dados são acumulados e descarregados em lotes (batching), reduzindo em até 2000 vezes as comutações Ring 3 ◄► Ring 0.
*   **Organização POSIX-like:** Segregação de APIs nos cabeçalhos padrão `<stdio.h>` e `<string.h>`.
*   **Interface Syscall Unificada:** Roteamento central das system calls de usuário baseadas na chamada inline `_syscall()` de 6 argumentos via registradores em conformidade com a convenção SysV ABI.

### 🛡️ Endurecimento e Segurança do Kernel (Sprint 2) — v4.2-sec
*   **CR0.WP (Write Protect) e EFER.NXE (NX Bit):** Proteção de gravação em páginas Ring 0 e suporte a bit Não-Executável (No-Execute) aplicados via hardware.
*   **Política W^X (Write XOR Execute):** Segregação estrita onde páginas graváveis (heap, stack, data) possuem `PAGE_NX` ativado, e páginas executáveis (`.text`) são mantidas como somente-leitura.
*   **Kernel Stack Guard Pages:** Páginas de proteção não-presentes na base das pilhas de kernel de 8 KiB para captura imediata de estouro de pilha (Stack Overflow) via `#PF`.
*   **Stack Canary (-fstack-protector-strong):** Inserção de canários de pilha pelo compilador com proteção e panic dedicado (`__stack_chk_fail`) no kernel. Os binários Ring 3 freestanding não usam canário até que o runtime forneça TLS/FS-base por tarefa.
*   **Sanitização e Validação do Heap:** Detecção de Double Free, UAF Poisoning (preenchimento com byte veneno `0xDD`) e verificador de integridade da lista encadeada (`heap_validate`).
*   **Validação Estrita de Ponteiros de Syscall:** Verificação de permissões e mapeamento de ponteiros de usuário (`vmm_validate_user_ptr`/`vmm_validate_user_string`) impedindo dereferenciamento indevido de memória kernel.

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
*   **Correção de consistência v4.1:** Após uma divisão COW, o inventário de frames da tarefa passa a registrar o novo frame antes de liberar a referência do antigo; isso evita que a finalização de um processo libere memória ainda mapeada pelo processo irmão. O handler aceita COW somente para uma falha de proteção por escrita (`PRESENT|WRITE`) em PTE marcada `PAGE_COW`.
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

Para o registro da auditoria e das correções de Page Fault/COW, consulte [VMM_COW_PAGEFAULT_FIXES_v4.1.md](docs/VMM_COW_PAGEFAULT_FIXES_v4.1.md).
