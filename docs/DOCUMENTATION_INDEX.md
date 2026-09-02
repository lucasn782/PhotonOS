# 📚 Índice Geral de Documentação Técnica do PhotonOS

Este documento serve como o mapa central da documentação técnica do PhotonOS v4.2.1, categorizando cada arquivo explicativo e especificações técnicas de acordo com seu assunto e subsistema.

---

## 🎯 Arquivos de Controle e Gerenciamento

### 0. [PHOTONOS_V4.2.1_BASELINE_AUDIT.md](PHOTONOS_V4.2.1_BASELINE_AUDIT.md)
*   **Descrição**: Relatório completo de auditoria de baseline, hardening, sincronização SMP e validação de regressões do PhotonOS v4.2.1.
*   **Objetivo**: Consolidar as evidências técnicas comprovadas em testes (10/10 boots, VFS, SMP stress, Pipes, Redirecionamento, TCP, Persistência) para preparar o início da v4.3.
*   **Dependências**: Todas as camadas do sistema operacional.
*   **Público-Alvo**: Engenheiros principais, arquitetos de sistemas e revisores de código.

### 1. [ARCHITECTURAL_DECISIONS.md](ARCHITECTURAL_DECISIONS.md)
*   **Descrição**: Registro de decisões de design técnico críticas tomadas no projeto.
*   **Objetivo**: Justificar as escolhas arquiteturais (ex: por que usar Copy-On-Write, mutexes atômicos, spinlocks seguros para interrupções e printf bufferizado).
*   **Dependências**: Nenhuma.
*   **Público-Alvo**: Arquitetos de software, revisores de código e novos desenvolvedores.

### 2. [CHANGELOG.md](CHANGELOG.md)
*   **Descrição**: Histórico completo de alterações no PhotonOS por versão.
*   **Objetivo**: Rastrear novos recursos, correções de bugs, breaking changes e impactos arquiteturais em cada release.
*   **Dependências**: Nenhuma.
*   **Público-Alvo**: Todos os desenvolvedores e usuários do PhotonOS.

### 3. [ROADMAP.md](ROADMAP.md)
*   **Descrição**: Planejamento e visão futura do desenvolvimento do PhotonOS.
*   **Objetivo**: Dividir o status entre tarefas concluídas, em andamento, planejadas e débitos técnicos prioritários.
*   **Dependências**: Nenhuma.
*   **Público-Alvo**: Desenvolvedores e engenheiros interessados em contribuir para o sistema.

### 4. [KNOWN_ISSUES.md](KNOWN_ISSUES.md)
*   **Descrição**: Catálogo de limitações, bugs conhecidos e débitos técnicos.
*   **Objetivo**: Documentar de forma transparente os limites atuais do kernel (ex: TCP sem fluxo de dados, fila de teclado em SMP e screen tearing).
*   **Dependências**: Nenhuma.
*   **Público-Alvo**: Desenvolvedores e engenheiros de integração.

### 5. [KERNEL_SECURITY.md](KERNEL_SECURITY.md)
*   **Descrição**: Especificação de arquitetura de segurança do núcleo (Sprint 2).
*   **Objetivo**: Detalhar validação de ponteiros de syscall, canário de pilha (-fstack-protector-strong), sanitização de Heap (Double-Free/UAF) e Guard Pages.
*   **Dependências**: VMM, Heap, Syscalls.
*   **Público-Alvo**: Engenheiros de segurança e mantenedores do kernel.

### 6. [MEMORY_PROTECTION.md](MEMORY_PROTECTION.md)
*   **Descrição**: Proteção de memória virtual e execução via hardware (Sprint 2).
*   **Objetivo**: Detalhar CR0.WP, EFER.NXE, política W^X, invalidação TLB e barreiras de sincronização.
*   **Dependências**: VMM, Paginação 64-bit.
*   **Público-Alvo**: Engenheiros de sistemas e segurança.

### 7. [VFS.md](VFS.md)
*   **Descrição**: Arquitetura e infraestrutura do Virtual File System (Sprint 3).
*   **Objetivo**: Detalhar abstração de nós, resolução recursiva de symlinks e chamadas de hard link.
*   **Dependências**: VFS, EXT2, FAT16.
*   **Público-Alvo**: Engenheiros de sistemas de arquivos.

### 8. [MOUNT_MANAGER.md](MOUNT_MANAGER.md)
*   **Descrição**: Especificação do Mount Manager e Tabela Global de Mounts (Sprint 3).
*   **Objetivo**: Detalhar redirecionamento transparente de caminhos e gerenciamento de múltiplos volumes.
*   **Dependências**: VFS, drivers de armazenamento.
*   **Público-Alvo**: Engenheiros de kernel e armazenamento.

### 9. [PERMISSIONS.md](PERMISSIONS.md)
*   **Descrição**: Modelo de Permissões POSIX Simplificadas (Sprint 3).
*   **Objetivo**: Detalhar UID, GID, bits octais de modo, verificação de acesso, `chmod` e `chown`.
*   **Dependências**: VFS, Escalonador.
*   **Público-Alvo**: Engenheiros de segurança e kernel.

### 10. [VMM_COW_PAGEFAULT_FIXES_v4.1.md](VMM_COW_PAGEFAULT_FIXES_v4.1.md)
*   **Descrição**: Registro da auditoria do VMM, Copy-On-Write, Page Fault e ciclo de vida de tarefas da v4.1.
*   **Objetivo**: Documentar a causa raiz corrigida, os critérios de resolução COW, a política de guard pages, as hipóteses descartadas e a validação executada.
*   **Dependências**: PMM, VMM, Scheduler, TSS/IDT e SMP.
*   **Público-Alvo**: Mantenedores de memória virtual e engenheiros de kernel.

### 11. [BOOT_INITIALIZATION_FIX_v4.2.md](BOOT_INITIALIZATION_FIX_v4.2.md)
*   **Descrição**: Relatório completo de recuperação do boot, resolução de Triple Fault, inicialização antecipada da IDT e realocação da BSS na v4.2.
*   **Objetivo**: Documentar a causa raiz exata da regressão no boot, a colisão de BSS na memória VGA 0xA0000, as correções no Heap/VMM/SMP e o layout de memória atualizado.
*   **Dependências**: Bootloader, GDT, IDT, PMM, VMM, Heap, SMP, Framebuffer.
*   **Público-Alvo**: Engenheiros de kernel, especialistas de boot e mantenedores do sistema.

### 12. [BOOT.md](BOOT.md)
*   **Descrição**: Especificação técnica e arquitetura do processo de bootloader multiestágio do PhotonOS.
*   **Objetivo**: Detalhar layout da imagem (LBA 0/LBA 1..480), carregamento LBA em 4 pacotes DAP com respeito ao limite de 64 KiB do Real Mode, A20, VBE VESA, GDT, paginação de 128 MiB e salto para Long Mode 64-bit.
*   **Dependências**: `src/boot/boot.asm`, `src/boot/kernel.asm`, `Makefile`.
*   **Público-Alvo**: Engenheiros de sistemas, especialistas em assembly x86 e desenvolvedores de kernel.

### 13. [BOOT_TROUBLESHOOTING.md](BOOT_TROUBLESHOOTING.md)
*   **Descrição**: Guia de resolução de problemas de boot, histórico de regressão LBA e interpretação de traces do QEMU.
*   **Objetivo**: Documentar a causa raiz do limite de 64 KiB em transferências BIOS LBA, o efeito secundário no fallback CHS em discos rígidos (DL=0x80), a correção em 4 janelas e como diferenciar eventos de firmware/SMM de exceções do PhotonOS no `task_debug.log`.
*   **Dependências**: QEMU, SeaBIOS, Bootloader.
*   **Público-Alvo**: Desenvolvedores e engenheiros de depuração.

### 14. Subsistema TCP v4.2 / v4.4 & Arquitetura de Rede
*   **[networking/tcp_phase2a.md](networking/tcp_phase2a.md)**: Especificação completa da **TCP Phase 2A** (3-Way Handshake `SYN -> SYN+ACK -> ACK`, máquina de estados `ESTABLISHED`, temporizadores RTO, tratamento de RST, timeout, integração `connect()` e validação por captura PCAP no fio).
*   **[tcp_socket_integration_fix.md](tcp_socket_integration_fix.md)**: Relatório da correção da integração `sys_socket()` com a camada de Sockets TCP no boot, causa raiz (`current_task == NULL`), alocação de descritores no contexto do kernel e testes de não-regressão.
*   **[tcp_architecture.md](tcp_architecture.md)**: Arquitetura completa do subsistema TCP, estrutura dos PCBs, máquina de estados, serialização/checksum, fluxos RX/TX e diagrama completo da pilha de rede.
*   **[network_architecture.md](network_architecture.md)** (e **[networking/network_architecture.md](networking/network_architecture.md)**): Arquitetura global da pilha de rede, barramento PCI, DMA físico e suporte a sockets.
*   **[tcp_socket_layer.md](tcp_socket_layer.md)**: Abstração VFS, nós de socket, mapeamento de syscalls (`connect`, `listen`, `accept`) e desativação de espera ocupada no escalonador.
*   **[tcp_pcb.md](tcp_pcb.md)**: Estrutura detalhada do Protocol Control Block (`struct tcp_pcb`), filas de segmentos dinâmicos e temporizadores.
*   **[tcp_port_management.md](tcp_port_management.md)**: Gerenciamento de portas bem conhecidas e efêmeras (49152..65535), prevenção de colisão e liberação.
*   **[tcp_checksum.md](tcp_checksum.md)**: Especificação do checksum TCP RFC 793 com pseudo-cabeçalho IPv4 e complemento de 1 em soma de 16 bits.
*   **[tcp_design.md](tcp_design.md)**: Princípios de design, desacoplamento modular, ausência de busy-wait e roadmap de evolução.
*   **[tcp_state_machine.md](tcp_state_machine.md)**: Especificação da máquina de estados TCP RFC 793 (10 estados).
*   **Público-Alvo**: Arquitetos de sistemas operacionais, engenheiros de rede e desenvolvedores do kernel.

### 15. Sinais POSIX, Processos e IPC v4.3
*   **[process_lifecycle.md](process_lifecycle.md)**: Arquitetura completa do ciclo de vida de processos, máquina de estados estrita (`TASK_READY`, `TASK_RUNNING`, `TASK_SLEEPING`, `TASK_WAITING`, `TASK_BLOCKED`, `TASK_STOPPED`, `TASK_ZOMBIE`, `TASK_DEAD`), gerador de PID monotônico, reparenting automático de órfãos para PID 1 e colheita atômica por `waitpid(WNOHANG)`.
*   **[signals.md](signals.md)**: Arquitetura do subsistema de sinais POSIX, tabela de sinais, bitmasks de bloqueio/pendência, trampoline em Ring 3 (`SIGNAL_TRAMPOLINE_ADDR`), preservação de contexto e retorno seguro com `sigreturn`.
*   **[process_signals.md](process_signals.md)**: Guia de desenvolvimento e uso das chamadas de sistema `sigaction`, `sigprocmask`, `kill`, e exemplos em Ring 3 para captura de `SIGCHLD`, `SIGPIPE`, `SIGSTOP` e `SIGCONT`.
*   **[IPC.md](IPC.md)**: Arquitetura de Comunicação Inter-Processos (IPC), pipes anônimos com contagem de leitores e escritores ativos, detecção de Broken Pipe (`SIGPIPE`), sinalização de EOF (`0`) e sincronização de buffer circular.
*   **[SCHEDULER.md](SCHEDULER.md)**: Escalonador preemptivo Round-Robin multi-core (SMP), gerenciamento atômico de estados de tarefas, preempção por timer e entrega assíncrona de sinais na transição para Ring 3.
*   **Público-Alvo**: Engenheiros de kernel, desenvolvedores de sistemas operacionais e programadores de userspace.

---

## 🏗️ 1. Arquitetura do Núcleo (architecture/)

### 1.1. [architecture.md](architecture/architecture.md)
*   **Descrição**: Design de privilégios de hardware e segmentação da GDT/TSS.
*   **Objetivo**: Explicar a separação Ring 0 / Ring 3, os seletores da GDT, e a prevenção de Triple Faults via pilhas IST.
*   **Dependências**: GDT e TSS do kernel.
*   **Público-Alvo**: Engenheiros de kernel.

### 1.2. [boot_process.md](architecture/boot_process.md)
*   **Descrição**: Fluxo de boot e inicialização multiestágio.
*   **Objetivo**: Detalhar a transição do processador de Modo Real (16-bit) → Modo Protegido (32-bit) → Modo Longo (64-bit) com paginação ativa.
*   **Dependências**: Bootloader assembly (`boot.asm` / `kernel.asm`).
*   **Público-Alvo**: Especialistas em boot e assembly x86.

### 1.3. [process_lifecycle.md](architecture/process_lifecycle.md)
*   **Descrição**: Mecanismos de criação e destruição de tarefas.
*   **Objetivo**: Explicar chamadas de sistema `sys_fork`, `sys_execve`, `sys_exit` e `sys_wait`, e liberação de recursos.
*   **Dependências**: Memória virtual (VMM) e Escalonador.
*   **Público-Alvo**: Engenheiros de sistemas operacionais.

### 1.4. [scheduler.md](architecture/scheduler.md)
*   **Descrição**: Escalonador preemptivo e exclusão mútua em tabelas críticas.
*   **Objetivo**: Detalhar a fila Round-Robin, estados de tarefas, preempção por timer e spinlocks seguros contra interrupções (`task_table_lock`).
*   **Dependências**: APIC / PIT Timer, interrupções.
*   **Público-Alvo**: Engenheiros de sistemas operacionais.

### 1.5. [smp.md](architecture/smp.md)
*   **Descrição**: Protocolo de bootstrap multi-core (Symmetric Multiprocessing).
*   **Objetivo**: Descrever a varredura ACPI MADT, inicialização de Application Processors (APs), IPIs de controle e TLB Shootdown.
*   **Dependências**: Local APIC (LAPIC), ACPI.
*   **Público-Alvo**: Especialistas em multiprocessamento.

### 1.6. [apic.md](architecture/apic.md)
*   **Descrição**: Registradores e operações do Local APIC.
*   **Objetivo**: Detalhar o mapeamento MMIO base, inicialização, desativação de PIC legado e interrupções locais.
*   **Dependências**: MSRs do processador.
*   **Público-Alvo**: Programadores de hardware.

### 1.7. [ioapic.md](architecture/ioapic.md)
*   **Descrição**: Roteamento físico de IRQs pelo chip I/O APIC.
*   **Objetivo**: Explicar o endereçamento de interrupções de hardware para núcleos de CPU por meio da tabela de redirecionamento.
*   **Dependências**: Local APIC, IDT.
*   **Público-Alvo**: Engenheiros de drivers.

### 1.8. [interrupts.md](architecture/interrupts.md)
*   **Descrição**: Tabela de Descritores de Interrupção (IDT).
*   **Objetivo**: Detalhar stubs assembly de tratamento, alinhamento de pilhas para a ABI, e isolamento de falhas do Ring 3.
*   **Dependências**: Arquitetura x86_64.
*   **Público-Alvo**: Engenheiros de kernel.

### 1.9. [syscall_flow.md](architecture/syscall_flow.md)
*   **Descrição**: Roteador e manipulação de System Calls de 64 bits.
*   **Objetivo**: Descrever o funcionamento das instruções `syscall`/`sysret` via MSRs e passagem de parâmetros.
*   **Dependências**: IDT/GDT, ulibc.
*   **Público-Alvo**: Engenheiros de compiladores e ulibc.

---

## 🧠 2. Gerenciamento de Memória (memory/)

### 2.1. [pmm.md](memory/pmm.md)
*   **Descrição**: Alocador de Memória Física baseada em Bitmap.
*   **Objetivo**: Descrever o bitmap que rastreia páginas físicas de 4 KiB e as funções `pmm_alloc` / `pmm_free`.
*   **Dependências**: Memória física disponível.
*   **Público-Alvo**: Engenheiros de memória.

### 2.2. [vmm.md](memory/vmm.md)
*   **Descrição**: Mapeamento Virtual Hierárquico de 4 níveis.
*   **Objetivo**: Detalhar travessia PML4 → PDPT → PD → PT, isolamento de áreas kernel/usuário e `vmm_lock`.
*   **Dependências**: PMM.
*   **Público-Alvo**: Engenheiros de memória virtual.

### 2.3. [paging.md](memory/paging.md)
*   **Descrição**: Configurações de paginação de hardware.
*   **Objetivo**: Mapear as flags da tabela de páginas (Present, Writable, User, Cache Disable, Write Through).
*   **Dependências**: VMM.
*   **Público-Alvo**: Programadores de baixo nível.

### 2.4. [cow.md](memory/cow.md)
*   **Descrição**: Otimização Copy-On-Write (COW).
*   **Objetivo**: Detalhar contadores de referência (`pmm_refcounts`), interceptação de falha por escrita no Page Fault, divisão de frames compartilhados e invalidação de TLB.
*   **Dependências**: PMM, VMM, IPIs.
*   **Público-Alvo**: Desenvolvedores de alta performance.

### 2.5. [heap.md](memory/heap.md)
*   **Descrição**: Alocador dinâmico do Kernel (Heap).
*   **Objetivo**: Descrever o alocador First-Fit com lista ligada (`struct heap_block`), divisão/fusão de blocos e `heap_lock`.
*   **Dependências**: VMM, PMM.
*   **Público-Alvo**: Desenvolvedores de Ring 0.

### 2.6. [tlb.md](memory/tlb.md)
*   **Descrição**: Coerência de caches de tradução de endereços.
*   **Objetivo**: Explicar a invalidação de TLB local por instruções `invlpg` e por recarga do registrador `CR3`.
*   **Dependências**: VMM.
*   **Público-Alvo**: Especialistas em arquitetura de processador.

---

## 🗄️ 3. Sistemas de Arquivos (filesystem/)

### 3.1. [vfs.md](filesystem/vfs.md)
*   **Descrição**: Camada de Abstração do Virtual File System (VFS).
*   **Objetivo**: Detalhar as estruturas de nós e a interface abstrata comum para montagem e acesso a arquivos.
*   **Dependências**: Nenhuma.
*   **Público-Alvo**: Desenvolvedores de arquivos e E/S.

### 3.2. [fat16.md](filesystem/fat16.md)
*   **Descrição**: Driver do Sistema de Arquivos FAT16 gravável.
*   **Objetivo**: Explicar a validação do BPB, navegação em diretório 8.3, redimensionamento de cadeias de clusters e controle por `fat16_mutex`.
*   **Dependências**: VFS, Driver ATA.
*   **Público-Alvo**: Engenheiros de armazenamento.

### 3.3. [ext2.md](filesystem/ext2.md)
*   **Descrição**: Driver do Sistema de Arquivos EXT2 nativo gravável.
*   **Objetivo**: Detalhar superblocos, descritores de grupo, leitura/escrita de inodes, ponteiros indiretos, bitmaps e divisão de entradas de diretório.
*   **Dependências**: VFS, Driver ATA.
*   **Público-Alvo**: Engenheiros de armazenamento e file systems.

---

## 🌐 4. Pilha de Rede (networking/)

### 4.1. [network_architecture.md](networking/network_architecture.md)
*   **Descrição**: Topologia e pipeline das camadas de rede.
*   **Objetivo**: Explicar a comunicação entre o driver de rede, a thread de rede do kernel e os buffers de sockets.
*   **Dependências**: e1000 Driver.
*   **Público-Alvo**: Engenheiros de rede.

### 4.2. [e1000.md](networking/e1000.md)
*   **Descrição**: Driver para a placa Ethernet Intel e1000 (PCI).
*   **Objetivo**: Explicar o endereçamento base MMIO, anéis de buffers de descritores DMA de transmissão (TX) e recepção (RX), e IRQ 11.
*   **Dependências**: PCI Bus, DMA.
*   **Público-Alvo**: Desenvolvedores de drivers de rede.

### 4.3. [arp.md](networking/arp.md)
*   **Descrição**: Protocolo de Resolução de Endereços (ARP).
*   **Objetivo**: Detalhar o cache do ARP, requisições por broadcast e resolução lógica de endereços MAC.
*   **Dependências**: Ethernet, IP.
*   **Público-Alvo**: Engenheiros de rede de baixo nível.

### 4.4. [ipv4.md](networking/ipv4.md)
*   **Descrição**: Protocolo de Internet Versão 4.
*   **Objetivo**: Descrever validações de cabeçalhos, cálculo de checksums IP e despacho de datagramas.
*   **Dependências**: Ethernet.
*   **Público-Alvo**: Engenheiros de rede.

### 4.5. [icmp.md](networking/icmp.md)
*   **Descrição**: Tratamento de Echo Request/Reply (Ping).
*   **Objetivo**: Explicar o encapsulamento, checksums ICMP e suporte a sockets raw (`SOCK_RAW`).
*   **Dependências**: IPv4, Sockets.
*   **Público-Alvo**: Engenheiros de diagnóstico.

### 4.6. [udp.md](networking/udp.md)
*   **Descrição**: Protocolo de Datagramas de Usuário (UDP).
*   **Objetivo**: Detalhar a montagem de cabeçalhos, checksums com pseudo-cabeçalho IP e filas de recepção do socket.
*   **Dependências**: IPv4, Sockets.
*   **Público-Alvo**: Engenheiros de rede.

### 4.7. [tcp.md](networking/tcp.md)
*   **Descrição**: Protocolo de Controle de Transmissão (Handshake).
*   **Objetivo**: Detalhar o estabelecimento de conexão (Three-Way Handshake SYN → SYN-ACK → ACK), o TCB local e as limitações de envio de dados.
*   **Dependências**: IPv4, Sockets.
*   **Público-Alvo**: Engenheiros de rede e arquitetos.

---

## 🔌 5. Drivers de Hardware (drivers/)

### 5.1. [framebuffer.md](drivers/framebuffer.md)
*   **Descrição**: Driver de Vídeo VBE (VESA Bios Extensions).
*   **Objetivo**: Explicar o mapeamento LFB em alta memória com cache write-through, double-buffering e desenho do cursor por software.
*   **Dependências**: Parâmetros do bootloader.
*   **Público-Alvo**: Engenheiros de interface e vídeo.

### 5.2. [keyboard.md](drivers/keyboard.md)
*   **Descrição**: Driver de Teclado PS/2 (IRQ 1).
*   **Objetivo**: Detalhar tabelas US-QWERTY, códigos Make e Break, buffers de entrada e interceptação Ctrl+C para sinal SIGINT.
*   **Dependências**: IDT, Escalonador.
*   **Público-Alvo**: Engenheiros de drivers de entrada.

### 5.3. [mouse.md](drivers/mouse.md)
*   **Descrição**: Driver de Mouse PS/2 (IRQ 12).
*   **Objetivo**: Detalhar a inicialização do chip 8042, parsing de pacotes de dados de 3 bytes e limites de coordenadas gráficas.
*   **Dependências**: IDT, Vídeo Framebuffer.
*   **Público-Alvo**: Engenheiros de drivers.

### 5.4. [ata.md](drivers/ata.md)
*   **Descrição**: Driver de Disco ATA (PIO Mode).
*   **Objetivo**: Explicar leitura/escrita LBA de 28 bits por portas de E/S de 16 bits e segurança contra concorrência via `ata_mutex`.
*   **Dependências**: VFS, barramento IDE.
*   **Público-Alvo**: Desenvolvedores de drivers de armazenamento.

### 5.5. [pci.md](drivers/pci.md)
*   **Descrição**: Varredura ativa do barramento PCI.
*   **Objetivo**: Descrever o acesso ao espaço de configuração pelas portas `0xCF8`/`0xCFC`, leitura de BARs de 32/64 bits e ativação de barramento DMA.
*   **Dependências**: Hardware PCI.
*   **Público-Alvo**: Desenvolvedores de barramento e drivers.

---

## 👥 6. Espaço de Usuário e ulibc (userspace/)

### 6.1. [shell.md](userspace/shell.md)
*   **Descrição**: Console de comandos interativo.
*   **Objetivo**: Descrever os comandos embutidos, pipelines de pipes (`|`) e redirecionamentos de arquivos (`>`) via `dup2`.
*   **Dependências**: ulibc, VFS.
*   **Público-Alvo**: Desenvolvedores de aplicações.

### 6.2. [libc.md](userspace/libc.md)
*   **Descrição**: Biblioteca padrão de espaço de usuário (ulibc).
*   **Objetivo**: Descrever o gateway central de system calls `_syscall()`, o alocador dinâmico malloc/free do heap de Ring 3 e o printf bufferizado de 2 KiB.
*   **Dependências**: Interface de syscalls do kernel.
*   **Público-Alvo**: Desenvolvedores C no PhotonOS.

### 6.3. [elf_loader.md](userspace/elf_loader.md)
*   **Descrição**: Motor de carregamento de executáveis ELF64.
*   **Objetivo**: Explicar a validação de cabeçalhos, mapeamento de segmentos PT_LOAD na PML4, stack de usuário, trampolim de sinais e compactação `-N` (OMAGIC).
*   **Dependências**: VMM, PMM, VFS.
*   **Público-Alvo**: Engenheiros de kernel e compiladores.

### 6.4. [programs.md](userspace/programs.md)
*   **Descrição**: Catálogo de programas Ring 3 em disco.
*   **Objetivo**: Catalogar os binários `shell`, `ping`, `hello`, `hang`, `upper`, `rev` e `spin`, detalhando seu código de teste e casos de validação.
*   **Dependências**: ulibc.
*   **Público-Alvo**: Desenvolvedores e usuários finais.
