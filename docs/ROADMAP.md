# 🗺️ PhotonOS — Roadmap do Sistema

Este documento descreve o estado atual do desenvolvimento do PhotonOS, dividindo as iniciativas entre marcos concluídos, tarefas em andamento, planejamentos futuros, débitos técnicos identificados e problemas conhecidos.

---

## 🟢 Concluído

### Trilha 1 a 4 — Core & Memory (v1.0)
- **Bootloader Multiestágio:** Inicialização em Modo Real (16-bit) -> Modo Protegido (32-bit) -> Entrada em Modo Longo de 64-bit.
- **Gerenciamento de Memória Física (PMM):** Alocador baseado em bitmap para páginas de 4 KiB.
- **Gerenciamento de Memória Virtual (VMM):** Configuração de tabelas PML4 de 4 níveis com suporte a mapeamentos kernel/usuário.
- **Kernel Heap:** Alocação dinâmica de memória no kernel com suporte a `kmalloc()` e `kfree()`.

### Trilha 5 e 6 — Processos & Armazenamento Básico (v2.0)
- **Escalonador Preemptivo:** Algoritmo Round-Robin preemptivo baseado nas interrupções do temporizador (PIT).
- **Espaço de Usuário (Ring 3):** Separação de privilégios com suporte a chamadas de sistema (`syscall`/`sysret`) e TSS.
- **Carregador de Binários ELF:** Execução de binários ELF64 dinamicamente a partir do filesystem.
- **Virtual File System (VFS):** Abstração unificada de caminhos e arquivos.
- **Driver de Disco ATA PIO:** Leitura e escrita direta em disco via portas de E/S.
- **Filesystem FAT16:** Leitura recursiva de caminhos no sistema de arquivos FAT16.

### Trilha 7 — Gráficos & Rede (v2.0 / v2.1)
- **Pipeline Gráfico VBE:** Modo de vídeo 1024x768 com 32bpp, Linear Framebuffer (LFB) e técnica de Double Buffering.
- **PCI Bus Scanning:** Detecção automática de dispositivos PCI baseada em classe e subclasse.
- **Driver Ethernet Intel e1000:** Operação de rede baseada em anéis de descritores de DMA (RX/TX) em Ring 0.
- **Sockets BSD:** Abstração de socket no kernel com chamadas de sistema `socket`, `bind`, `connect`, `send` e `recv` não-bloqueantes (`-EAGAIN`).

### Trilha 8 — Multiprocessamento Simétrico (v3.0)
- **Arquitetura Multi-Core (SMP):** Descoberta e inicialização de núcleos Application Processors (APs) através da sequência de IPIs INIT-SIPI-SIPI.
- **Local APIC (LAPIC):** Desativação do PIC legado e gerenciamento direto do LAPIC por núcleo.
- **Sincronização:** Spinlocks atômicos baseados nas primitivas integradas GCC (`__sync_lock_test_and_set`).

### Trilha 9 — Otimização Copy-On-Write (v3.1)
- **Copy-On-Write (COW):** Compartilhamento de frames físicos no `sys_fork` com separação dinâmica via Page Fault (`INT 0x0E`).
- **Contador de Referências no PMM:** Estrutura thread-safe para rastrear o compartilhamento de páginas de memória.
- **TLB Shootdown:** Invalidação coordenada de caches de tradução de endereços (TLB) em múltiplos processadores via IPI no Vetor `0x79`.

### Trilha 10 — EXT2 Gravável & Blindagem (v4.0)
- **Filesystem EXT2 Nativo Gravável:** Suporte a leitura, escrita e criação de caminhos recursivos.
- **Blindagem do Driver ATA:** Adição de exclusão mútua (`ata_mutex`) para acesso concorrente SMP seguro ao hardware de disco.

### Trilha 11 — ulibc Refactor & POSIX Hardening (v4.1)
- **Refatoração da Biblioteca de Usuário (ulibc):** Printf bufferizado (buffer de 2048 bytes), cabeçalhos `<stdio.h>` e `<string.h>`, e wrappers POSIX de system calls.
- **Descoberta Dinâmica de CPU via ACPI MADT**: Parsing das tabelas RSDP, RSDT e MADT no boot para identificar cores de CPU habilitados.
- **Isolamento de Falhas Ring 3**: Terminação limpa de processos usuários causadores de exceções (#GP / #PF) em vez de Kernel Panic.
- **Sincronização de Documentação**: Auditoria completa do código-fonte e reorganização da pasta `docs/`.

### Trilha 12 — Kernel Security Hardening & Memory Protection (v4.2-sec)
- **Proteção por Hardware CR0.WP & EFER.NXE**: Ativação dos bits de Write Protection (CR0.WP) e No-Execute (NX Bit) em 64-bit Long Mode.
- **Política W^X (Write XOR Execute)**: Mapeamento de páginas graváveis (heap, stack, data) com `PAGE_NX`, e páginas executáveis (`.text`) como somente-leitura.
- **Kernel Stack Guard Pages**: Guard Pages não-presentes na base das pilhas de kernel de 8 KiB para captura imediata de estouro de pilha.
- **Stack Canary (-fstack-protector-strong)**: Proteção contra estouro de pilha no kernel com `__stack_chk_guard` e `__stack_chk_fail`. A cobertura Ring 3 fica pendente da inicialização de TLS/FS-base por tarefa.
- **Sanitização do Heap**: Detecção de Double Free, UAF Poisoning (`0xDD`) e verificador `heap_validate()`.
- **Validação Estrita de Syscalls**: Verificação de ponteiros de usuário (`vmm_validate_user_ptr`/`vmm_validate_user_string`) em todas as system calls.

### Trilha 13 — VFS Completo, Permissões & Mount Manager (v4.3-fs)
- **Permissões POSIX Simplificadas**: Controle de acesso por bits octais (`0755`/`0644`), `uid` e `gid` de processos e arquivos, `chmod()` e `chown()`.
- **Hard Links & Symlinks**: `link()`, `unlink()`, refcounting `nlink`, `symlink()`, `readlink()` e resolução recursiva de symlinks (`vfs_find_following_symlinks`).
- **Mount Manager & Tabela Global de Mounts**: Estrutura `vfs_mount_t`, lista `vfs_mount_list`, chamadas `vfs_mount`/`vfs_umount` e travessia transparente de múltiplos volumes (`mounted_here`).

### Trilha 14 — Sinais POSIX, Ciclo de Vida de Processos & IPC (v4.3)
- **Subsistema de Sinais POSIX**: Suporte a sinais assíncronos (`SIGINT`, `SIGKILL`, `SIGPIPE`, `SIGTERM`, `SIGCHLD`, `SIGCONT`, `SIGSTOP`, `SIGTSTP`), `sigaction`, `sigprocmask` e trampoline em Ring 3.
- **Ciclo de Vida de Processos**: Máquina de estados estrita (`TASK_READY`, `TASK_RUNNING`, `TASK_STOPPED`, `TASK_ZOMBIE`, `TASK_DEAD`), gerador de PID monotônico, reparenting automático para PID 1 e `waitpid(WNOHANG)`.
- **IPC por Pipes Anônimos**: Buffer circular de 4 KiB, contagem de leitores/escritores, detecção de Broken Pipe (`SIGPIPE`) e retorno EOF (`0`).

### Trilha 15 — Estabilização do Bootloader & Bugfix de Janela LBA (v4.3-boot)
- **Fragmentação LBA em 4 Janelas (480 setores)**: Carregamento do kernel particionado em pacotes DAP de no máximo 128 setores (64 KiB), eliminando overflow de offset de 16 bits no Modo Real.
- **Eliminação de Dependência de CHS em Hard Disk**: Validação estrita de extensões BIOS EDD (`INT 13h, AH=42h`) para execução segura em discos rígidos (`DL=0x80`).
- **Resolução de Boot Regression**: Estabilização comprovada com 10/10 boots consecutivos com passagem em todos os subsistemas.

### Trilha 16 — TCP Phase 2A: 3-Way Handshake & Conexão Ativa (v4.4-tcp2a)
- **3-Way Handshake Ativo (RFC 793):** Sequência completa `SYN -> SYN+ACK -> ACK` validada por inspeção de pacotes PCAP no fio (*wire*).
- **Máquina de Estados Operacional:** Transições estritas `CLOSED -> SYN_SENT -> ESTABLISHED`, tratamento de `RST` (porta fechada) e timeout.
- **Syscall `connect()` com Bloqueio Seguro:** Integração com a camada de sockets sem busy-wait infinito, adormecendo a tarefa chamadora no escalonador.
- **Temporizadores RTO e Prevenção de Deadlocks:** Retransmissão com recuo exponencial e transmissão diferida no `tcp_timer_tick()`, desacoplada de `tcp_pcbs_lock`.
- **Concorrência e Estresse:** Validação de 4 conexões consecutivas e 4 conexões simultâneas via `fork()` sem corrupção ou vazamento de recursos.

---

## 🟡 Em Desenvolvimento (v4.4-dev)

### Trilha 17 — TCP Phase 2B: Streams & Passive Open (v4.4)
- **Transmissão e Recepção de Dados (`send`/`recv`):** Transferência de fluxo contínuo sobre conexões `ESTABLISHED` integradas ao VFS (`read`/`write`).
- **Abertura Passiva do Servidor (`listen`/`accept`):** Fila de conexões pendentes (*backlog*) e geração de sockets derivados.
- **Encerramento Ordenado de Conexão:** Transições `FIN`, `FIN+ACK`, `TIME_WAIT` e `CLOSED`.

---

## 🔵 Planejado

### Espaço de Usuário Avançado (v4.5)
- **Suporte a Execução de Scripts ELF Avançados e Variáveis de Ambiente:** Expansão da ulibc e suporte a argumentos e variáveis de ambiente no `sys_execve`.

### Sincronização e IPC (v4.6)
- **Semáforos e Variáveis de Condição:** Sincronização avançada no escalonador para bloqueio e desbloqueio eficiente de threads.

### Servidor HTTP em Ring 3 (v4.7)
- **Servidor Web Demonstrativo:** Aplicação executando em espaço de usuário (Ring 3) respondendo a conexões HTTP GET na porta 80.

### Utilitários do Sistema (v4.8)
- **Comandos de Usuário Adicionais:** Utilitários `chmod`, `chown`, `ln`, `mount`, `umount`, `top`, `netstat`.

---

## 🧹 Débito Técnico

1. **Timeout do TLB Shootdown:** 
   - *Status:* Crítico em SMP.
   - *Descrição:* O loop `while (tlb_acknowledge_count < active_aps)` no clone de espaço de endereçamento do VMM não possui limite de tempo. Se um núcleo AP sofrer um travamento por hardware ou interrupções desativadas, o núcleo BSP ficará em deadlock infinito aguardando confirmação.
2. **Sincronização na Fila de Teclado:**
   - *Status:* Moderado.
   - *Descrição:* O buffer circular `keyboard_queue` em `src/kernel/kernel.c` é lido e escrito por diferentes threads sem spinlocks de proteção. Embora a preempção de interrupções atenue o risco, o acesso multiprocessador (SMP) direto violaria a consistência concorrente do buffer circular.
3. **Otimização do Page Fault Handler COW:**
   - *Status:* Baixo.
   - *Descrição:* A cópia de dados no handler COW é feita por um loop byte-a-byte de 4096 iterações. Substituir por uma rotina otimizada de cópia por blocos de 64 bits ou instruções x86 `rep movsq` trará ganhos expressivos na performance do `sys_fork`.
4. **Duplicação de Código em Validação de Sinais:**
   - *Status:* Baixo.
    - *Descrição:* A função de validação `is_supported_signal()` está duplicada em `src/kernel/kernel.c` e `src/kernel/scheduler.c`. Deve ser movida para um arquivo de utilidades comuns ou header unificado.
5. **TLS para Ring 3 e Canário de Pilha:**
    - *Status:* Moderado.
    - *Descrição:* Os processos freestanding ainda não possuem FS-base/TLS. O canário do compilador foi desabilitado para Ring 3 para evitar acesso a `FS:0x28`; reativá-lo requer salvar, restaurar e inicializar FS-base por tarefa.

---

## ⚠️ Problemas Conhecidos

1. **Assinatura de ABI Inconsistente:** A mudança de `size_t count` para `int count` nas chamadas `read()` e `write()` da ulibc causa uma divergência formal com o padrão POSIX, embora não tenha impacto funcional sob os limites do sistema operacional atual.
2. **Poli-alocação de buffer gráfico sob stress:** Múltiplas atualizações sucessivas do console gráfico podem resultar em lag perceptível devido à transferência direta do backbuffer para o Linear Framebuffer sem sincronização vertical (V-Sync).
