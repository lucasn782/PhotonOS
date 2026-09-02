# PhotonOS v4.3 🚀

O **PhotonOS v4.3** é um sistema operacional monolítico freestanding desenvolvido do zero para a arquitetura **x86_64**, executando em **64-bit Long Mode**. O projeto consolida um núcleo multi-core preemptivo robusto com Copy-On-Write (COW), descoberta dinâmica de CPUs via ACPI MADT, isolamento de falhas do Ring 3, pipeline gráfico por software, barramento PCI, interface de rede Intel e1000 estável, armazenamento persistente dual (FAT16 + EXT2), biblioteca padrão de usuário (`ulibc`) otimizada com printf bufferizado, subsistema de protocolo TCP (Fase 1 e Fase 2A: 3-Way Handshake e connect ativo), uma camada VFS expandida e o subsistema de **Sinais POSIX, Ciclo de Vida de Processos com Reparenting e File Descriptors com Pipe IPC**.

---

## 🚀 Funcionalidades Consolidadas

### ⚡ Sinais POSIX, Ciclo de Vida de Processos & IPC (v4.3)
*   **Subsistema de Sinais POSIX:** Suporte a sinais assíncronos (`SIGINT`, `SIGKILL`, `SIGPIPE`, `SIGTERM`, `SIGCHLD`, `SIGCONT`, `SIGSTOP`, `SIGTSTP`), gerenciamento de máscaras bloqueadas (`sigprocmask`), registro de tratadores customizados (`sigaction`), desvio de contexto em Ring 3 via trampoline (`SIGNAL_TRAMPOLINE_ADDR` / `sys_sigreturn`) e ações padrão do kernel.
*   **Máquina de Estados de Processos Estrita:** Ciclo de vida robusto com estados `TASK_UNUSED`, `TASK_READY`, `TASK_RUNNING`, `TASK_SLEEPING`, `TASK_WAITING`, `TASK_BLOCKED`, `TASK_STOPPED`, `TASK_ZOMBIE` e `TASK_DEAD`.
*   **Gerador de PID Monotônico:** Alocação estritamente crescente e livre de reutilização prematura de PIDs sob spinlocks com IRQ salva.
*   **Reparenting Automático e SIGCHLD:** Ao encerrar, os processos órfãos são automaticamente adotados pelo PID 1 (shell/init), e o processo pai recebe a notificação `SIGCHLD` de forma atômica.
*   **Colheita Atômica por `waitpid`:** Suporte a `waitpid(pid, status, options)` com flag `WNOHANG` e transição atômica de `TASK_ZOMBIE` para `TASK_DEAD`, prevenindo Use-After-Free e vazamentos de descritores.
*   **IPC com Pipes e Detecção de Broken Pipe:** Buffer circular de 4 KiB com contagem atômica de leitores (`readers`) e escritores (`writers`). Emissão automática de `SIGPIPE` e retorno `-1` em escritas sem leitores ativos, e retorno `0` (EOF) em leituras quando todos os escritores fecharam.

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

### 🌐 Subsistema TCP (Fase 1 & Fase 2A — v4.4-tcp2a)
*   **Fundação Estrutural (Fase 1):** Módulo de controle de protocolo (`src/kernel/tcp.c`, `include/tcp.h`) com gerenciamento de PCBs em lista global protegida por mutex, cálculo de checksum RFC 793 com pseudo-cabeçalho IPv4, alocação de portas efêmeras IANA (49152–65535), demultiplexação por 4-tuple e integração com `socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)` e `bind()`.
*   **Three-Way Handshake & Conexão Ativa (Fase 2A):** Implementação ativa do handshake completo (`SYN -> SYN+ACK -> ACK`) comprovado no fio (*wire*) via captura PCAP, máquina de estados operacional (`CLOSED -> SYN_SENT -> ESTABLISHED`), tratamento de `RST` e timeout de conexão.
*   **Syscall `connect()` Cooperativa:** Bloqueio no escalonador via `scheduler_sleep_current(TASK_WAIT_NETWORK)` e `scheduler_yield()`, permitindo que o sistema continue executando outras tarefas e processando pacotes de rede sem espera ocupada ("zero busy-wait").
*   **Temporizadores RTO e Prevenção de Impasses:** Retransmissão com recuo exponencial e fila de transmissão diferida (*deferred queue*) em `tcp_timer_tick()`, prevenindo deadlocks durante o envio de pacotes.
*   **Próxima Etapa (Fase 2B):** Abertura passiva (`listen`/`accept`) e fluxo bidirecional de dados (`send`/`recv` / integração VFS read/write).

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

*   **Marcos Concluídos**: Inicialização multiestágio 64-bit com janela LBA fragmentada (480 setores), alocador PMM/VMM com W^X, escalonador preemptivo Round-Robin multi-core (SMP), pipeline gráfico VBE com Double Buffering, driver de rede e1000 com ICMP/ARP e sockets BSD, Copy-On-Write (COW), sistemas de arquivos FAT16 e EXT2 graváveis, VFS com symlinks/hardlinks/permissões, printf bufferizado na ulibc, subsistema TCP (Fase 1 e Fase 2A: 3-Way Handshake, connect ativo e PCAP wire inspection) e subsistema de Sinais POSIX com IPC via Pipes anônimos.
*   **Em Desenvolvimento (v4.4-dev)**: TCP Fase 2B (transmissão/recepção de dados via sockets de fluxo, passive open `listen`/`accept`), e tratamento de ICMP Port Unreachable para datagramas UDP sem socket.
*   **Planejado**: Suporte a execução de scripts e variáveis de ambiente no userspace, semáforos/condvars no escalonador e servidor HTTP em Ring 3.

Para documentação técnica detalhada de cada módulo, consulte o [Índice de Documentação Técnica](docs/DOCUMENTATION_INDEX.md).

Para a especificação técnica do bootstrap e carregamento do kernel, consulte [BOOT.md](docs/BOOT.md).

Para histórico de troubleshooting, causa raiz de limites de 64 KiB em BIOS LBA e análise de traces do QEMU, consulte [BOOT_TROUBLESHOOTING.md](docs/BOOT_TROUBLESHOOTING.md).

Para o registro de recuperação de boot e realocação da BSS na v4.2, consulte [BOOT_INITIALIZATION_FIX_v4.2.md](docs/BOOT_INITIALIZATION_FIX_v4.2.md).
