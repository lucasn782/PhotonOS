# 📝 PhotonOS — Changelog

Histórico completo de mudanças do sistema operacional, organizado por versão.
Convenções: cada entrada lista data, commit (quando aplicável), resumo, arquivos alterados, bugs corrigidos, novas funcionalidades, breaking changes e impacto arquitetural.

---

## `v4.3-fs` — Filesystem Infrastructure, Permissions & Mount Manager (Sprint 3) 📂
**Data:** 2026-07-22
**Status:** Released

### Novas Funcionalidades
- **Permissões POSIX Simplificadas (mode_t):** Controle de acesso baseado em bits octais (`0755`/`0644`), bitmasks `S_IRWXU`/`S_IRWXG`/`S_IRWXO` e validação preventiva `vfs_check_permission()`.
- **Gerenciamento de Identidade (UID & GID):** Suporte a propriedade de arquivos/diretórios por usuário (`uid`) e grupo (`gid`), herdados por tarefas via `fork()`/`spawn()`.
- **Chamadas `chmod()` e `chown()`:** Suporte a alteração dinâmica de permissões octais e propriedades via `sys_chmod` e `sys_chown`.
- **Hard Links (`link()`, `unlink()`):** Criação de múltiplos hard links compartilhando nós e contagem de referências físicas `nlink` com desalocação em `nlink == 0`.
- **Symbolic Links (`symlink()`, `readlink()`):** Suporte a nós do tipo `VFS_NODE_SYMLINK`, armazenamento do caminho de destino em `symlink_target` e resolução recursiva `vfs_find_following_symlinks()` limitada a profundidade 8 (`ELOOP`).
- **Mount Manager & Tabela Global de Mounts:** Estrutura `vfs_mount_t` e lista encadeada `vfs_mount_list` permitindo a montagem/desmontagem dinâmica de múltiplos volumes (`vfs_mount`/`vfs_umount`).
- **Navegação Transparente de Múltiplos Volumes:** Redirecionamento automático `mounted_here` no `vfs_find()`, permitindo cruzar fronteiras entre múltiplos sistemas de arquivos montados (ex: FAT16 e EXT2).

### Arquivos Alterados
| Arquivo | Tipo | Resumo |
|---------|------|--------|
| `include/vfs.h` | Refatorado | Adição de `VFS_NODE_SYMLINK`, constantes `S_IRWX...`, `uid/gid/mode/nlink` em `vfs_node_t` e `vfs_dir_entry_t`, `vfs_mount_t` e prototypes VFS |
| `src/kernel/vfs.c` | Refatorado | Implementação de `vfs_chmod`, `vfs_chown`, `vfs_link`, `vfs_unlink`, `vfs_symlink`, `vfs_readlink`, `vfs_mount`, `vfs_umount` e `vfs_check_permission` |
| `include/task.h` | Atualizado | Inclusão de `uid` e `gid` na estrutura `task_control_block` |
| `src/kernel/scheduler.c` | Atualizado | Herança e inicialização de `uid` e `gid` nas tarefas criadas |
| `src/kernel/kernel.c` | Atualizado | Adição das constantes `SYS_CHMOD` a `SYS_UMOUNT` (27-34) e despacho no `syscall_handler` com validação de ponteiros |
| `include/ulibc.h` | Atualizado | Protótipos das chamadas de usuário `chmod`, `chown`, `link`, `unlink`, `symlink`, `readlink`, `mount`, `umount` |
| `src/user/ulibc.c` | Atualizado | Implementação dos wrappers inline POSIX de chamada de sistema |
| `docs/VFS.md` | **Novo** | Especificação da arquitetura e abstração do Virtual File System |
| `docs/MOUNT_MANAGER.md` | **Novo** | Especificação do Mount Manager e Tabela Global de Mounts |
| `docs/PERMISSIONS.md` | **Novo** | Especificação do modelo de permissões POSIX simplificadas |

---

## `v4.2-sec` — Kernel Security Hardening & Memory Protection (Sprint 2) 🛡️
**Data:** 2026-07-22
**Status:** Released

### Novas Funcionalidades
- **CR0.WP (Write Protect):** Ativação por hardware do bit 16 do registrador `CR0`. O código executando em Ring 0 não pode mais gravar em páginas marcadas como Read-Only.
- **NX Bit & EFER.NXE:** Ativação do bit 11 no MSR `IA32_EFER` (`0xC0000080`), ativando a aplicação do bit 63 (`PAGE_NX`) nas entradas de tabelas de páginas de 64 bits.
- **Política W^X (Write XOR Execute):** Segregação estrita onde páginas graváveis (heap, stack, data) possuem `PAGE_NX` ativado, e páginas executáveis (`.text`) são mantidas como somente-leitura.
- **Kernel Stack Guard Pages:** Pilhas de kernel de cada tarefa expandidas para 8 KiB (`TASK_STACK_SIZE`), com uma Guard Page não-presente (`PAGE_PRESENT = 0`) no endereço inferior. Estouro de pilha resulta em exceção `#PF` controlada sem corromper estruturas adjacentes.
- **Stack Canary (-fstack-protector-strong):** Habilitação de canários de pilha do compilador com guardião `__stack_chk_guard` e manipulador de pânico `__stack_chk_fail` no kernel. O canário Ring 3 permanece desabilitado até que TLS/FS-base seja inicializado por tarefa.
- **Sanitização e Validação do Heap:** Detecção de Double Free com log/abort seguro, UAF Poisoning (preenchimento com byte veneno `0xDD`) e verificador de integridade `heap_validate()`.
- **Validação Estrita de Syscalls:** Verificação preventiva de ponteiros de usuário (`vmm_validate_user_ptr`/`vmm_validate_user_string`) impedindo dereferenciamento indevido de memória restrita ao kernel ou ponteiros nulos/inválidos.

### Arquivos Alterados
| Arquivo | Tipo | Resumo |
|---------|------|--------|
| `include/vmm.h` | Atualizado | Definição de `PAGE_NX`/`VMM_PAGE_NX`, prototypes de `CR0.WP`, `EFER.NXE` e validação de ponteiros de usuário |
| `src/kernel/vmm.c` | Refatorado | Implementação de `CR0.WP`, `EFER.NXE`, `PAGE_NX`, `vmm_validate_user_ptr`, `vmm_validate_user_string` e log aprimorado de `#PF` |
| `src/boot/kernel.asm` | Atualizado | Ativação precoce no boot de `EFER.NXE` (bit 11 MSR `0xC0000080`) e `CR0.WP` (bit 16 `CR0`) |
| `src/boot/boot.asm` | Atualizado | Ajuste de `KERNEL_SECTORS` para 352 setores para imagens blindadas de até 176 KiB |
| `include/task.h` | Atualizado | Adição do campo `guard_page` na estrutura `task_control_block` |
| `src/kernel/scheduler.c` | Atualizado | Expansão de pilhas para 8 KiB com Guard Pages não-presentes na base |
| `Makefile` | Atualizado | `-fstack-protector-strong` para kernel, `-fno-stack-protector` para Ring 3 sem TLS e ajuste de `KERNEL_SECTORS := 352` |
| `src/kernel/kernel.c` | Atualizado | Definição de canário de pilha e validação de ponteiros em todas as chamadas de sistema no `syscall_handler` |
| `src/user/ulibc.c` | Atualizado | Runtime de usuário; canário permanece desabilitado até haver TLS/FS-base |
| `include/heap.h` | Atualizado | Protótipo `heap_validate(void)` |
| `src/kernel/heap.c` | Refatorado | `PAGE_NX` no heap, detecção de Double Free, UAF Poisoning (`0xDD`) e `heap_validate()` |
| `docs/KERNEL_SECURITY.md` | **Novo** | Especificação completa da arquitetura de segurança do núcleo |
| `docs/MEMORY_PROTECTION.md` | **Novo** | Especificação técnica de proteção de memória por hardware |

### Impacto Arquitetural
- O kernel PhotonOS torna-se uma plataforma altamente segura com proteção de memória baseada em hardware (WP, NX, W^X).
- Proteção completa contra estouro de pilha no kernel (canários + guard pages).
- Erros de ponteiro em chamadas de sistema oriundos de Ring 3 agora falham graciosamente retornando `-1` (`EFAULT`) sem derrubar o kernel.

---

## `v4.1` — The ulibc Refactor, POSIX Hardening & Documentation Sync Update 📝
**Data:** 2026-07-21
**Status:** Released

### Novas Funcionalidades
- **Printf Bufferizado:** Reescrita completa do `printf()` userspace com buffer interno de 2048 bytes (`struct printf_buffer`), reduzindo chamadas de sistema de N (uma por caractere) para ⌈N/2048⌉ (uma por flush). Ganho de performance estimado: 100-500x em strings longas.
- **APIs POSIX Padronizadas:** Novas funções `open()`, `read()`, `write()`, `close()`, `fork()` com assinaturas compatíveis POSIX usando `_syscall` de 6 argumentos.
- **Headers `string.h` e `stdio.h`:** Criação de headers separados para funções de string (`memcpy`, `memset`, `strlen`, `strcmp`) e I/O (`printf`), seguindo convenção POSIX.
- **Wrapper Syscall Unificado:** `_syscall()` inline com 6 argumentos via registradores SysV ABI (rdi, rsi, rdx, r10, r8, r9).
- **Descoberta Dinâmica de CPU via ACPI MADT**: O kernel agora busca o RSDP no BIOS ou EBDA para localizar a tabela MADT do ACPI, descobrindo as CPUs dinamicamente.
- **Isolamento de Falhas Ring 3**: Exceções GPF (#GP) ou Page Fault (#PF) geradas em Ring 3 agora finalizam o processo ofensivo via `scheduler_exit_current(-1)` de forma limpa em vez de causar pânico geral no kernel.
- **Auditoria e Sincronização Completa de Documentação**: Reorganização integral da pasta `docs/` dividida por subsistemas (`architecture/`, `memory/`, `filesystem/`, `networking/`, `drivers/`, `userspace/`), remoção de arquivos duplicados/obsoletos, criação de novos índices de controle e atualização de todos os links relativos.

### Arquivos Alterados
| Arquivo | Tipo | Resumo |
|---------|------|--------|
| `src/user/ulibc.c` | Refatorado | Printf bufferizado, novas APIs POSIX |
| `include/ulibc.h` | Atualizado | Novas declarações open/read/write/close/fork, includes string.h/stdio.h |
| `include/stdio.h` | **Novo** | Declaração de `printf()` |
| `include/string.h` | **Novo** | Declarações de `memcpy/memset/strlen/strcmp` |
| `include/apic.h` | Atualizado | Novos registradores APIC (LVT_PERF, LVT_LINT0/1, LVT_ERR) |
| `include/smp.h` | Atualizado | Exportação de `tlb_acknowledge_count` e `tlb_shootdown_addr` |
| `src/kernel/smp.c` | Melhorado | Melhorias no bootstrap AP, suporte a ACPI MADT e carregamento TR/IDT |
| `src/kernel/vmm.c` | Melhorado | Ajustes no COW clone, isolamento de falhas do Ring 3 |
| `src/kernel/kernel.c` | Ajustado | Integração com novos headers e inicialização ACPI |
| `src/kernel/net.c` | Ajustado | Include adicional |
| `src/kernel/scheduler.c` | Ajustado | Integração APIC |
| `src/kernel/trampoline.asm` | Ajustado | Melhorias no boot AP |
| `Makefile` | Atualizado | Ajustes de dependências |
| `docs/` | Reorganizado / Novo | Reorganização geral de toda a documentação do projeto |

### Breaking Changes
- ⚠️ Assinatura de `read()` e `write()` mudou de `size_t count` para `int count` no header público `ulibc.h`
- ⚠️ As funções legadas `syscall0`–`syscall4` agora são wrappers sobre `_syscall` (sem impacto funcional)

### Impacto Arquitetural
- A ulibc agora segue uma arquitetura em camadas: `_syscall` → wrappers POSIX → funções de conveniência → printf bufferizado
- Separação de concerns: string operations (`string.h`), I/O (`stdio.h`), system calls (`ulibc.h`)
- O bootstrap de APs agora carrega IDT e TR para garantir tratamento de interrupções e integridade de transições de privilégios.
- Os APs agora mantêm interrupções ativas (`sti; hlt`) no loop ocioso, prevenindo deadlocks durante TLB Shootdowns do BSP.

---

## `v4.0` — The EXT2 Persistent Storage Update 📦
**Data:** 2026-07-01
**Commit:** `06a1d22`

### Novas Funcionalidades
- Sistema de Ficheiros EXT2 Nativo Gravável em Ring 0
- Blindagem concorrente no driver ATA (`ata_mutex`)
- Parser de Superbloco com validação do mágico `0xEF53`
- Carregamento da BGDT em RAM
- Conversão matemática de inodes via `ext2_read_inode()`/`ext2_write_inode()`
- Lookup recursivo de caminhos via VFS
- Alocação atômica de blocos e inodes
- Pipeline de escrita com ponteiros diretos e indiretos
- Divisão de entradas de diretório
- Detecção automática FAT16 → EXT2 fallback

### Arquivos Alterados
- `src/fs/ext2.c` (792 linhas adicionadas)
- `include/fs/ext2.h` (126 linhas adicionadas)
- `src/drivers/ata.c` (28 linhas modificadas)
- `Makefile` (10 linhas modificadas)
- `README.md` (48 linhas modificadas)
- `docs/DOCUMENTATION_INDEX.md` (56 linhas modificadas)
- `docs/ext2_filesystem.md` (336 linhas adicionadas)

### Impacto Arquitetural
- Novo subsistema de filesystem em `src/fs/` com driver EXT2 completo
- Driver ATA agora protegido por mutex para SMP safety
- VFS expandido com fallback automático de detecção de filesystem

---

## `v3.1` — The COW Memory Optimization Update 🧠
**Data:** 2026-06-24
**Commit:** `59739f0`

### Novas Funcionalidades
- Copy-On-Write (COW) para `sys_fork`
- Contador de referências no PMM (`pmm_refcounts`)
- Handler de Page Fault COW (`INT 0x0E`)
- TLB Shootdown via LAPIC (Vector `0x79`)
- Flags de PTE customizadas (`PAGE_COW = 0x200`)

### Bugs Corrigidos
- Eliminação de duplicação física desnecessária durante fork
- Correção de memory leaks em processos que fazem fork sem escrever

### Arquivos Alterados
- `src/kernel/vmm.c` (126 linhas adicionadas)
- `include/vmm.h` (3 linhas adicionadas)
- `src/kernel/memory.c` (43 linhas adicionadas)
- `include/memory.h` (2 linhas adicionadas)
- `src/boot/kernel.asm` (96 linhas adicionadas)
- `docs/cow_memory_optimization.md` (439 linhas adicionadas)

### Impacto Arquitetural
- VMM agora é stateful com rastreamento de referências por frame
- Page fault handler expandido com lógica COW
- SMP impactado: TLB shootdown obrigatório após modificação de PTEs

---

## `v3.0` — The SMP Update 🚀
**Data:** 2026-06-24
**Commit:** `6e052ea`

### Novas Funcionalidades
- Multiprocessamento Simétrico (SMP) com suporte a 4 cores
- Ecossistema APIC nativo (desativação do PIC 8259)
- Código trampolim em `0x7000` para bootstrap de APs
- Spinlocks atômicos via `__sync_lock_test_and_set`
- Pilhas isoladas por núcleo
- TLB Shootdown handler (Vector `0x79`)
- Socket BSD API (`sys_socket`, `sys_bind`, `sys_connect`)

### Arquivos Alterados
- `src/kernel/smp.c` (340 linhas adicionadas)
- `src/kernel/apic.c` (66 linhas adicionadas)
- `src/kernel/trampoline.asm` (135 linhas adicionadas)
- `src/kernel/net.c` (665 linhas adicionadas)
- `include/smp.h`, `include/apic.h` (criados)
- `docs/smp.md` (202 linhas adicionadas)

### Impacto Arquitetural
- Kernel agora é multi-core: todo estado compartilhado precisa de proteção
- APIC substitui PIC como controlador primário de interrupções
- Novo vetor de interrupção `0x79` para TLB shootdown

---

## `v2.0` — The Graphics & Networking Update 🌌
**Data:** 2026-06-24
**Commit:** `fdceee4`

### Novas Funcionalidades
- Pipeline gráfico VBE (1024x768x32bpp) com Double Buffering
- Driver e1000 PCI de rede com DMA
- Sockets UDP e ICMP (ping)
- Mouse driver com sprite de seta
- Console adaptativo gráfico
- `sys_fork` com deep-copy PML4

### Arquivos Alterados
- 22 arquivos, 1837 inserções, 336 deleções

### Impacto Arquitetural
- Novo subsistema gráfico com framebuffer mapeado em high memory
- Stack de rede completa (Ethernet → IP → ICMP/UDP)
- PCI bus scanning e driver model

---

## `v1.1` — Socket Hardening & FAT16 Write
**Data:** 2026-06-16
**Commit:** `8a5b7de`

### Novas Funcionalidades
- Thread-safe ring buffers com cli/sti
- Validação de checksums IPv4, ICMP e UDP
- Socket reads não-bloqueantes (`-EAGAIN`)
- Resolução de colisões de headers (net.h vs sys/socket.h)
- FAT16 cluster writing e `sys_write` hardening

### Bugs Corrigidos
- Colisão de macros de endianness entre `net.h` e `sys/socket.h`
- Race conditions em ring buffers de sockets

---

## `v1.0` — The Core 64-bit Update ⚙️
**Data:** 2026-06-01
**Commit:** `6ce60d0`

### Novas Funcionalidades
- Bootloader x86 Assembly (Real Mode → Protected Mode → Long Mode)
- Tabelas de paginação PML4
- GDT/IDT/TSS
- PMM bitmap-based
- VMM com 4-level page tables
- Kernel Heap (`kmalloc`/`kfree`)
- VFS com FAT16 e initrd
- Escalonador Round-Robin preemptivo (PIT)
- Processos em Ring 3 com `syscall`/`sysret`
- ELF loader de 64-bit
- Driver ATA PIO
- Driver Serial COM1
- Shell interativo

### Impacto Arquitetural
- Fundação completa do sistema operacional
- Arquitetura monolítica com subsistemas modulares
