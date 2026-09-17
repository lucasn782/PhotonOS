# 📝 PhotonOS — Changelog

Histórico completo de mudanças do sistema operacional, organizado por versão.
Convenções: cada entrada lista data, commit (quando aplicável), resumo, arquivos alterados, bugs corrigidos, novas funcionalidades, breaking changes e impacto arquitetural.

## `v4.4-tcp2b` — Milestone TCP Phase 2B: Passive Open, RX & TX Data Plane 🌐
**Data:** 2026-09-17
**Status:** Consolidado e validado em WSL/Ubuntu/QEMU com inspeção PCAP.

### Resumo do Milestone
Consolidação completa da Fase 2B do subsistema TCP do PhotonOS, cobrindo o ciclo de abertura passiva (Phase 2B.1), o plano de dados de recepção (Phase 2B.2A) e o plano de dados de transmissão (Phase 2B.2B). O subsistema foi integrado de ponta a ponta com a camada de sockets BSD, subsistema VFS, gerenciamento de memória virtual (VMM) e escalonador cooperativo/preemptivo.

### Funcionalidades Consolidadas
- **TCP Phase 2B.1 (Passive Open / Listen / Accept):** Implementação completa do estado `TCP_LISTEN`, syscalls `listen()` e `accept()`, fila de backlog com saturação e descarte controlado (`TCP_MAX_BACKLOG = 16`), ciclo de vida independente de child PCBs com tuplas de 4 elementos, demultiplexação de entrada prioritária e bloqueio cooperativo no escalonador (`TASK_WAIT_SOCKET_RECV`) com despertar imediato sem busy-wait.
- **TCP Phase 2B.2A (Receive Path / RX Buffer):** Buffer circular de recepção (`tcp_rx_buffer_t`) de 8192 bytes por conexão, ingestão in-order de payload (`seq == rcv_nxt`), avanço monotônico de `rcv_nxt`, cálculo dinâmico da advertised window (`rcv_wnd`), descarte com re-ACK de dados duplicados e fora de ordem, syscall `recv()` (SYS_RECV = 54) com suporte a leituras parciais, eliminação de lost wakeups e semântica POSIX de EOF (`TCP_CLOSE_WAIT` / `TCP_PCB_FLAG_EOF`) e RST (`TCP_PCB_FLAG_RESET`).
- **TCP Phase 2B.2B (Transmit Path / TX Buffer):** Buffer de transmissão (`tcp_tx_buffer_t`) de 8192 bytes com isolamento entre filas `unsent` e `unacked`, cópia segura de payload Ring 3 para memória pertencente ao kernel, segmentação em MSS 1460, avanço de `SND.NXT` e rastreamento de `SND.UNA`, reconhecimento cumulativo e parcial de dados, temporizador RTO de dados com backoff exponencial e limite de retransmissões (`TCP_MAX_DATA_RETRIES = 5`), e syscall `send()` (SYS_SEND = 55) com semântica de escrita parcial não-bloqueante.

### Validação de Integração e Regressões
- **Validações Específicas de RX:** Confirmação de bloqueio cooperativo (`recv_block`), recepção concorrente em múltiplas conexões (`recv_multi`), herança de descritor pós-`fork()` (`recv_fork`), leitura por descritor duplicado via `dup()` (`recv_dup`) e estabilidade em bateria de recepção repetida (5/5 iterações consecutivas aprovadas).
- **Validações Específicas de TX:** Segmentação MSS 1460, validação de buffers de 1 byte a 16384 bytes, tratamento de escrita parcial, inspeção wire de flags `ACK|PSH` e correspondência estrita de sequências no fio (`ACK == SEQ + LEN`).
- **Regressão Global do Sistema:** Passagem integral de 10/10 boots consecutivos (`test_10_boots.py`), SMP, VFS, sinais POSIX e pipes (`test_signals_suite.py`), persistência em disco FAT16, ciclo de vida de processos (`fork`, `exec`, `waitpid`), conectividade ICMP e suíte TCP Phase 2A (`test_tcp_phase2a.py`).

### Otimização de Compilação e Métricas de Build
- **Otimização `-Os`:** Ativação da flag `-Os` em CFLAGS no `Makefile`, reduzindo significativamente a pegada de memória do executável.
- **Tamanho do Kernel:** `build/photon.bin` compilado em 137.772 bytes contra o teto arquitetural de 245.760 bytes (`KERNEL_MAX_BYTES`), garantindo uma margem de segurança de 107.988 bytes livres.
- **Gate de Build:** Verificação automatizada `test $(stat -c%s build/photon.bin) -le 245760` mantida ativa como barreira obrigatória no Makefile.

---

## `v4.4-tcp2b2b` — TCP Phase 2B.2B: Transmit Path & `send()` 🌐
**Data:** 2026-09-15
**Status:** Validado localmente no WSL com QEMU e PCAP (2026-09-15).

### Novas Funcionalidades
- **`send()` / `SYS_SEND = 55`:** Wrapper ulibc e syscall que validam descritor, socket TCP ativo, estado `ESTABLISHED`, `flags`, ponteiro de usuário e intervalo completo antes da cópia.
- **TX State por PCB:** `tcp_tx_buffer_t` de 8192 bytes separa dados pendentes de dados enviados/não confirmados. Cada segmento mantém uma cópia própria do payload no kernel, sequência, offset, timestamp e contador de retransmissões.
- **Segmentação e Sequências:** Payload é segmentado em `TCP_DEFAULT_MSS = 1460`, transmitido com checksum TCP/pseudo-header IPv4 e avança `SND.NXT` somente quando passa para a fila `unacked`.
- **Processamento de ACK de Dados:** ACK cumulativo, parcial, duplicado, antigo e além de `SND.NXT` são tratados sem liberar dados indevidos. ACK válido avança `SND.UNA` e libera apenas os bytes confirmados.
- **Retransmissão Básica de Dados:** Timer RTO de dados, backoff limitado, retransmissão com o mesmo `SEQ` e encerramento/limpeza após `TCP_MAX_DATA_RETRIES`.
- **Testes TX:** A suíte `test_tcp_phase2b2_tx.py` e os subcomandos `tcptest send_*` cobrem payloads de 1 byte a 16384 bytes, segmentação, escrita parcial, múltiplas conexões, `fork`, `dup`, casos negativos e análise PCAP.

### Correções de Segurança e Concorrência
- **Capacidade TX Atômica:** A decisão de espaço disponível ocorre sob `pcb->lock`, prevenindo sobrecarga do buffer por envios concorrentes.
- **Sem I/O de Rede Sob Locks TCP:** O pacote é montado antes de liberar o lock; `net_send_ipv4()` é chamado sem `tcp_pcbs_lock` nem `pcb->lock`.
- **Ownership durante TX:** `send()` e a escrita VFS retêm `sock->mutex` enquanto emprestam o PCB a `tcp_send()`, serializando corretamente com o fechamento final (`sock -> pcb`) e removendo uma janela de use-after-free.
- **Limpeza Terminal:** RST recebido, timeout de retries e destruição de PCB liberam ambas as filas TX. O fechamento local remove o PCB da demultiplexação sem reentrar a rede sob o lock do socket, e o armazenamento estático de transmissão diferida do timer é serializado por mutex próprio.

---

## `v4.4-tcp2b2a` — TCP Phase 2B.2A: Receive Path & RX Buffer 🌐
**Data:** 2026-09-09
**Status:** Released

### Novas Funcionalidades
- **Buffer Circular de Recepção (RX Buffer):** Implementação de `tcp_rx_buffer_t` com capacidade de 8192 bytes por PCB TCP, suportando escrita e leitura circular com wrap-around sem cópias intermediárias redundantes.
- **Ingestão de Dados em `ESTABLISHED`:** Processamento estrito de segmentos com payload recebidos no estado `ESTABLISHED`. Avanço seguro e monotônico de `rcv_nxt` somente para dados em ordem (`seq == rcv_nxt`).
- **Emissão Imediata de ACK de Dados:** Resposta com pacote ACK contendo `ack_number = rcv_nxt` e janela atualizada após o armazenamento seguro dos dados no buffer circular.
- **Política de Descarte e Re-ACK de Duplicados e Fora de Ordem:** Descarte seguro do payload para segmentos duplicados (`seq < rcv_nxt`) e out-of-order (`seq > rcv_nxt`), com re-emissão de ACK duplicado contendo o `rcv_nxt` esperado para ressincronização do transmissor remoto.
- **Syscall `recv()` com Bloqueio Cooperativo (SYS_RECV = 54):** Syscall para consumo seguro de dados do RX buffer pelo espaço de usuário. Suporte a leituras completas e parciais, retorno de bytes lidos e atualização da janela de recepção anunciada.
- **Eliminação de Lost Wakeups:** Desativação atômica de interrupções via `pushcli`/`popcli` durante a verificação de buffer vazio e suspensão no escalonador (`TASK_WAIT_SOCKET_RECV`), assegurando que interrupções de rede concorrentes não percam o `scheduler_wake_matching_tasks`.
- **Validação Estrita de Memória em Syscall:** Validação completa de ponteiros e limites do usuário com `vmm_validate_user_ptr(buffer, len, 1)` prevenindo acessos nulos, ponteiros não mapeados e buffers de kernel.
- **Tratamento de EOF e RST:** Detecção de encerramento remoto (`FIN` -> `TCP_CLOSE_WAIT` e flag `TCP_PCB_FLAG_EOF`) retornando 0 (EOF padrão POSIX) na exaustão do buffer, e aborto da conexão (`RST` / `TCP_PCB_FLAG_RESET`) retornando erro `-1`.
- **Concorrência e Herança Pós-`fork()` e `dup()`:** Validação de compartilhamento e leitura concorrente de descritores clonados sem corrupção de buffer ou de PCBs.
- **Suite Completa de Testes Automatizados e PCAP:** Implementação de `scripts/test_tcp_phase2b2_rx.py` com 17 verificações automatizadas de wire e userspace, além de 7 subcomandos novos no binário Ring 3 `tcptest.elf`.

### Correções de Bugs (Bug Fixes)
- **Eliminação de Avanço Indevido de `rcv_nxt` no Preâmbulo de `tcp_input`:** Removida a atribuição incondicional `pcb->rcv_nxt = header->sequence + payload_len` no preâmbulo de `tcp_input()`, que corrompia o rastreamento de sequência antes de checagens de estado e duplicação.
- **Retorno de EOF em `socket_vfs_read()`:** Ajustada a integração VFS para retornar 0 em caso de socket em `TCP_CLOSE_WAIT` / `TCP_PCB_FLAG_EOF` com buffer esgotado.

---

## `v4.4-tcp2b1` — TCP Phase 2B.1: Passive Open, Listen, Accept & Backlog 🌐
**Data:** 2026-09-09
**Status:** Released

### Novas Funcionalidades
- **Abertura Passiva RFC 793 (Passive Open):** Suporte completo a servidores TCP no estado `LISTEN`, permitindo o recebimento de conexões remotas iniciadas por clientes externos.
- **Syscall `listen()` (SYS_LISTEN = 35):** Validação de descritores de socket `SOCK_STREAM`, verificação de porta local vinculada e saturação segura de backlog entre 1 e 16 (`TCP_MAX_BACKLOG`).
- **Syscall `accept()` com Bloqueio Cooperativo (SYS_ACCEPT = 36):** Desenfileiramento de conexões prontas do backlog e espera cooperativa no escalonador (`TASK_WAIT_NETWORK`) com despertar imediato (`tcp_socket_notify`) na chegada do `ACK` final (zero *busy-wait*).
- **Ciclo de Vida Independente de Child PCBs:** Ao receber um `SYN`, o listener aloca um PCB filho independente com tupla de 4 elementos, Initial Sequence Number (`ISS`) próprio e monotônico, transicionando para `SYN_RECEIVED` e emitindo `SYN+ACK`. O listener permanece indefinidamente em `LISTEN`.
- **Fila de Backlog com Prevenção de Saturação:** Enfileiramento de PCBs promovidos a `ESTABLISHED`. Caso o backlog esteja cheio, novos `SYN`s são descartados sem alocação órfã de PCBs.
- **Demultiplexação de Entrada Prioritária:** `tcp_lookup_locked` prioriza correspondências exatas de 4-tuple para conexões ativas (`ESTABLISHED`/`SYN_RECEIVED`) antes de repassar pacotes aos listeners.
- **Isolamento de Descritores Pós-`fork()`:** Validação de herança de descritores clonados pós-`accept()`, permitindo que o processo filho processe o socket cliente enquanto o pai continua aceitando conexões.
- **Suite de Testes Automatizados e PCAP:** Implementação de `scripts/test_tcp_phase2b_passive.py` e comandos `listen`, `server_multi`, `server_fork`, `errors` em `tcptest.c`, cobrindo 15 casos de teste e validando traços reais no PCAP.

### Correções de Bugs (Bug Fixes)
- **Eliminação de Double Free em `socket_vfs_close()`:** Removida chamada redundante de `kfree(node)` na liberação de sockets VFS, prevenindo corrupção no heap durante `close()`.
- **Inicialização Completa de `vfs_node_t`:** Garante zeramento integral da estrutura `vfs_node_t` em `sys_socket()` e `sys_accept()` antes da inicialização de campos específicos.

---

## `v4.4-tcp2a` — TCP Phase 2A: 3-Way Handshake, State Machine & Connect() 🌐
**Data:** 2026-09-02
**Status:** Released


### Novas Funcionalidades
- **Three-Way Handshake RFC 793:** Implementação ativa do handshake completo (`SYN -> SYN+ACK -> ACK`) com validação de números de sequência (`ISS`), números de reconhecimento (`ack_num`), flags e cálculo de checksum.
- **Integração com Syscall `sys_connect`:** Associação de endpoints de rede a sockets `SOCK_STREAM` (`IPPROTO_TCP`), alocação de portas efêmeras seguras (`49152..65535`) e bloqueio cooperativo através do escalonador sem consumo excessivo de CPU.
- **Temporizadores RTO e Prevenção de Impasses:** Retransmissão de SYN com recuo exponencial e fila de transmissão diferida (*deferred queue*) em `tcp_timer_tick()`, garantindo que nenhuma transmissão de rede ocorra sob posse do mutex global `tcp_pcbs_lock`.
- **Tratamento de RST e Timeout de Conexão:** Transição para `CLOSED` em caso de recepção de `RST` (porta remota fechada) ou expiração do temporizador de conexão de 2 segundos.
- **Suite de Testes e Wire Inspection:** Criação do script `scripts/test_tcp_phase2a.py` e binário `tcptest.elf`, validando 5 casos de teste e capturando 32 segmentos TCP em arquivo PCAP com 100% de conformidade.

### Correções de Bugs (Bug Fixes)
- **Prevenção de Corrupção de Argumentos em `elf_load_process`:** Ajuste no cálculo do `user_rsp` inicial em `src/kernel/elf.c` para prevenir que o frame de pilha de funções em Ring 3 sobreescreva a string de argumentos posicionada no topo da pilha.

---

## `v4.3` — POSIX Signals, Process Lifecycle & Pipe IPC ⚡
**Data:** 2026-09-01
**Status:** Released

### Novas Funcionalidades
- **Subsistema de Sinais POSIX:** Implementação de `sigaction`, `sigprocmask`, envio de sinais via `kill`/`sys_kill`, tratamento assíncrono em Ring 3 via trampoline (`SIGNAL_TRAMPOLINE_ADDR`) e retorno seguro `sys_sigreturn`.
- **Ciclo de Vida de Processos Robusto:** Estados `TASK_READY`, `TASK_RUNNING`, `TASK_SLEEPING`, `TASK_WAITING`, `TASK_BLOCKED`, `TASK_STOPPED`, `TASK_ZOMBIE`, `TASK_DEAD`, gerador monotônico de PID, reparenting automático de filhos órfãos para o PID 1 com emissão atômica de `SIGCHLD`, e colheita por `waitpid` com suporte a `WNOHANG`.
- **Pipes Anônimos (IPC):** Buffer circular de 4 KiB com rastreamento atômico de descritores de leitura e escrita, emissão automática de `SIGPIPE` e retorno `-1` em escritas em pipes órfãos, e sinalização de EOF (`0`) em leituras.
- **Camada VFS Expandida e Buffer Cache (`bcache`):** Buffer cache de 32 KiB (64 blocos), chamadas `getcwd`, `chdir`, caminhos relativos, `truncate`, `ftruncate`, `dup`, `dup2`, `fcntl` (`F_DUPFD`, `F_GETFL`, `F_SETFL`), `flock` e `umask`.

---

## `v4.2.2` — BIOS LBA 64 KiB Window Bootloader Stabilization & Recovery 🚀
**Data:** 2026-09-01
**Status:** Released

### Correções de Bugs (Bug Fixes)
- **Correção do Carregamento LBA no Bootloader BIOS:** Corrigida regressão em que o kernel ultrapassava 256 setores e o terceiro pacote DAP solicitava 224 setores em uma única operação `INT 13h, AH=42h`, excedendo o limite de 64 KiB do offset de 16 bits em Modo Real.
- **Fragmentação em 4 Janelas DAP de 128 Setores:** O carregamento de 480 setores (240 KiB) foi particionado em 4 janelas (`128 + 128 + 128 + 96` setores) nos segmentos `0x0800`, `0x1800`, `0x2800` e `0x3800`, eliminando qualquer overflow de buffer e garantindo leitura contínua em `0x08000`–`0x43FFF`.
- **Prevenção de Falha em Fallback CHS para Discos Rígidos:** Documentada e mitigada a premissa de geometria de disquete (`SECTORS_TRACK = 18`) que causava travamento em `disk_error` quando o dispositivo de boot era apresentado como disco rígido (`DL=0x80`).
- **Validação de Não-Regressão de Boot:** Validados 10 boots consecutivos automatizados (10/10 PASS), modo manual QEMU com `-d int,cpu_reset,guest_errors` e `make run-fat16`.

### Arquivos Alterados
| Arquivo | Tipo | Resumo |
|---------|------|--------|
| `src/boot/boot.asm` | Corrigido | Particionamento do LBA em 4 pacotes DAP de até 128 setores cada |
| `Makefile` | Atualizado | Ajuste de `KERNEL_SECTORS := 480` e validação estrita de tamanho |
| `docs/BOOT.md` | **Novo** | Especificação técnica completa da arquitetura do bootloader multiestágio |
| `docs/BOOT_TROUBLESHOOTING.md` | **Novo** | Análise de causa raiz da regressão LBA e guia de interpretação de traces do QEMU |
| `docs/architecture/boot_process.md` | Atualizado | Sincronização do fluxo de boot com a implementação real |
| `docs/DOCUMENTATION_INDEX.md` | Atualizado | Inclusão de referências para `BOOT.md` e `BOOT_TROUBLESHOOTING.md` |

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
