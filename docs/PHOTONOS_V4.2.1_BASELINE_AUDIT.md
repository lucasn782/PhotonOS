# 📑 PhotonOS v4.2.1 — Relatório de Auditoria de Baseline, Hardening e Preparação Técnica para v4.3

**Projeto:** PhotonOS  
**Versão Atual Auditada:** v4.2.1  
**Data da Auditoria:** 25 de Agosto de 2026  
**Status do Baseline:** APROVADO (100% dos testes e verificações com status PASS)  

---

## 1. Resumo Executivo

Esta auditoria estabelece a linha de base técnica (baseline) do **PhotonOS v4.2.1**, validando todas as camadas do sistema operacional antes da transição e planejamento da **v4.3**.

O sistema foi submetido a uma rigorosa esteira de testes automatizados e análises estáticas de código, cobrindo:
1. **Bootloader e Inicialização x86_64:** 10 boots consecutivos em ambiente SMP de 4 núcleos sem falhas, panics ou corrupções de memória.
2. **Camada de Memória (PMM, VMM, Heap):** Isolamento de Ring 0/Ring 3, proteção contra execução de dados (W^X / NX bit), Write Protect em Ring 0 (CR0.WP), páginas de guarda nas pilhas de kernel e sanitização do Heap.
3. **Sistemas de Arquivos e VFS:** Hierarquia de nós com contagem de referências (`ref_count`), remoção física e desalocação no FAT16 (`unlink`), Buffer Cache Unificado de 64 blocos com sincronização (`sync`), suporte a `umask`, bloqueio de arquivos (`flock`/`fcntl`), montagem dinâmica desacoplada para ext2 (`ext2_mount_at`) e comandos `mount`/`umount`.
4. **Processos, Shell e IPC:** Processos em Ring 3 com suporte a `fork()`, `wait()`, `execve()`, pipelines anônimos (`|`), redirecionamento de entrada/saída (`<`, `>`) e leitura de `cat` via `stdin`.
5. **Estresse e Sincronização SMP:** Teste massivo de concorrência com 4 processos filhos independentes executando operações simultâneas de I/O em núcleos distintos, comprovando a robustez dos spinlocks e mutexes.
6. **Rede e Sockets:** Teste de regressão da pilha TCP (Fase 1) e validação de ping ICMP com 0% de perda de pacotes.
7. **Persistência de Dados Dual-Boot:** Teste de gravação em disco no Boot 1 e validação de integridade pós-reboot no Boot 2 com 100% de conformidade.

---

## 2. Estado do Build

*   **Compilador / Linker:** GCC 64-bit freestanding (`-ffreestanding -m64 -nostdlib -mno-red-zone -fno-pic -fno-pie -fstack-protector-strong -Wall -Wextra -Iinclude`).
*   **Setores de Kernel Configurados:** 480 setores (245.760 bytes).
*   **Tamanho do Binário do Kernel (`build/photon.bin`):** 215.484 bytes (421 setores ocupados).
*   **Margem de Segurança (Headroom):** 30.276 bytes livres (59 setores) abaixo do limite de carga do bootloader Stage 1.
*   **Tamanho da Imagem Floppy (`build/photon.img`):** 1.474.560 bytes (1,44 MB padrão).
*   **Tamanho do Disco FAT16 (`build/disk.img`):** 16.777.216 bytes (16 MB).
*   **Status de Warnings e Erros:** 0 erros de compilação, 0 erros de linkagem, 0 warnings.

---

## 3. Estado do Boot

*   **Estabilidade de Inicialização:** 10 boots consecutivos executados no QEMU (`qemu-system-x86_64 -smp 4`).
*   **Taxa de Sucesso:** 10 / 10 (100% PASS).
*   **Sequência Validada:**
    1. Bootloader Real Mode (16-bit) -> Protected Mode (32-bit) -> Long Mode (64-bit).
    2. Ativação antecipada da IDT antes de qualquer subsistema dinâmico.
    3. Inicialização e inventário de memória física (PMM).
    4. Ativação de CR0.WP e EFER.NXE no VMM.
    5. Framebuffer VBE e console VGA.
    6. Heap do kernel com proteção contra Double-Free e UAF.
    7. Buffer Cache (bcache) e detecção de disco primário ATA.
    8. Montagem do volume de partição FAT16.
    9. Varredura do barramento PCI e inicialização do controlador Ethernet Intel e1000.
    10. Subsistema TCP (Fase 1) e tabela de sockets.
    11. Inicialização do LAPIC e desativação do PIC legado 8259.
    12. Trampolim AP (0x7000) e ativação dos 3 núcleos secundários (SMP).
    13. Carga do shell ELF em Ring 3 e inicialização do escalonador Round-Robin.

---

## 4. Estado do SMP (Symmetric Multiprocessing)

*   **Topologia:** 4 CPUs ativas (1 BSP + 3 APs).
*   **Descoberta:** ACPI MADT com fallback de segurança.
*   **Inicialização dos APs:** Sequência INIT-SIPI-SIPI com código trampolim isolado em `0x7000`.
*   **Distribuição de Carga:** Escalonador Round-Robin distribuindo threads e processos de usuário entre todos os núcleos.
*   **Isolamento de Serial:** `serial_lock` (spinlock com salvamento de flags de IRQ) protege o acesso à porta COM1 contra intercalação de caracteres.
*   **Teste de Estresse (`smptest`):** Criação concorrente de processos filhos realizando escritas, leituras, lseeks e unlinks paralelos nos 4 núcleos, com integridade 100% preservada.

---

## 5. Estado do VMM, PMM e Heap

*   **Layout de Memória Virtual do Processo Ring 3:**
    *   `0x0000008000001000` - `0x00000080006FFFFF`: Código ELF (.text), Dados (.rodata, .data), BSS e Heap dinâmico (`sys_brk`).
    *   `0x0000008000700000`: `SIGNAL_TRAMPOLINE_ADDR` (página isolada e mapeada como somente-leitura com syscall de retorno).
    *   `0x00000080007FC000` - `0x0000008000800000`: Pilha de usuário (4 páginas, 16 KiB) com topo em `ELF_USER_STACK_TOP`.
*   **Políticas de Proteção:**
    *   **W^X:** Páginas executáveis mantidas sem permissão de escrita; páginas de dados/heap/stack marcadas com `PAGE_NX`.
    *   **CR0.WP:** Ativado para impedir que Ring 0 sobrescreva páginas de usuário somente-leitura.
    *   **Validação de Ponteiros de Syscall:** `vmm_validate_user_ptr` e `vmm_validate_user_string` rejeitam acessos fora do espaço de usuário (`0x1000` a `VMM_USER_LIMIT`).
    *   **Páginas de Guarda:** Página não-presente na base de cada pilha de kernel de 8 KiB para contenção de stack overflow.

---

## 6. Estado do Virtual File System (VFS)

*   **Nós e Estrutura:** Nós `vfs_node_t` protegidos por `vfs_mutex`.
*   **Ciclo de Vida (`ref_count` & `nlink`):**
    *   `ref_count`: Contador de descritores de arquivos abertos.
    *   `nlink`: Contador de vínculos no sistema de arquivos.
    *   Desalocação física de nós ocorre estritamente quando `ref_count == 0 && nlink == 0`.
*   **Navegação e Caminhos:**
    *   Suporte completo a resolução canônica de caminhos absolutos (`/dir/file`) e relativos (`file`, `./file`, `../file`).
    *   Gerenciamento de diretório de trabalho corrente (`CWD`) por tarefa com syscalls `sys_chdir` e `sys_getcwd`.
*   **Permissões e Bloqueios:**
    *   `umask`: Mascaramento octal herdado no `fork()` e gerenciado via `sys_umask`.
    *   `flock` / `fcntl`: Bloqueios compartilhados/exclusivos (`LOCK_SH`, `LOCK_EX`, `LOCK_UN`) e manipulação de descritores (`F_DUPFD`, `F_GETFL`, `F_SETFL`).
*   **Sistemas de Arquivos Integrados:**
    *   **FAT16:** Suporte nativo com remoção física em `unlink`, criação de diretórios e persistência de dados.
    *   **EXT2:** Driver desacoplado com suporte a montagem dinâmica (`ext2_mount_at`) em pontos de montagem arbitrários via `sys_mount` e `sys_umount`.

---

## 7. Estado dos Processos e Escalonador

*   **Tabela de Tarefas:** 32 slots de tarefas gerenciados por `task_table_lock`.
*   **Estados de Tarefa:** `TASK_READY`, `TASK_RUNNING`, `TASK_SLEEPING`, `TASK_BLOCKED`, `TASK_ZOMBIE`.
*   **Syscalls de Ciclo de Vida:**
    *   `sys_fork`: Clona o espaço de endereçamento, descritores de arquivos abertos, CWD e umask. Retorna 0 para o filho e o PID do filho para o pai.
    *   `sys_wait`: Permite ao processo pai aguardar a conclusão do processo filho e coletar seu status de saída.
    *   `sys_exit`: Finaliza a tarefa e notifica o pai sem vazamentos de memória.
    *   `sys_execve`: Carrega binários ELF substituindo a imagem do processo corrente.

---

## 8. Estado dos File Descriptors e File Descriptions

*   **Arquitetura Compartilhada (`file_description_t`):**
    *   Cada processo possui um array `files[MAX_FILES_PER_TASK]` de ponteiros para objetos `file_description_t`.
    *   `file_description_t` mantém o offset de leitura/escrita, flags de abertura, contador de referências e ponteiro para o nó VFS (`node`).
*   **Semântica POSIX:**
    *   `dup(fd)` e `dup2(oldfd, newfd)` incrementam o `ref_count` da mesma `file_description_t`, garantindo que atualizações de offset sejam refletidas em todos os descritores duplicados.
    *   `fork()` duplica a tabela de descritores incrementando as referências das `file_description_t`, permitindo compartilhamento natural entre processos pai e filho.
    *   `close(fd)` decrementa a referência da descrição de arquivo e libera o nó VFS associado apenas quando a última referência é fechada.

---

## 9. Estado dos Pipes e Redirecionamentos

*   **Pipes Anônimos (`SYS_PIPE`):**
    *   Buffer circular em memória (`PIPE_BUFFER_SIZE`) com exclusão mútua (`pipe->lock`).
    *   Mecanismo de sleep/wakeup sem busy-wait para leitores e escritores (`scheduler_sleep_current(TASK_WAIT_PIPE_READ/WRITE)` e `scheduler_wake_pipe_readers/writers`).
*   **Shell Pipelines (`|`):**
    *   Despachante generalizado conectando o descritor de escrita do pipe à saída padrão (`stdout`) do comando produtor e o descritor de leitura à entrada padrão (`stdin`) do comando consumidor.
*   **Redirecionamentos (`>`, `<`):**
    *   Redirecionamento de saída (`>`) com criação/abertura automática do arquivo de destino.
    *   Redirecionamento de entrada (`<`) com abertura em leitura e redirecionamento do descritor 0 (`stdin`).
    *   Comando `cat` compatível com leitura direta de `stdin` em encadeamentos e pipes.

---

## 10. Estado da Rede

*   **Controlador Ethernet:** Driver Intel e1000 operando via barramento PCI e anéis DMA de recepção e transmissão.
*   **Protocolos Básicos:** IPv4, ARP e ICMP Echo Request/Reply operacionais (0% de perda de pacotes em testes de loopback e rede externa).
*   **Pilha TCP (Fase 1):**
    *   Controle de Protocolo (PCB) com lista encadeada protegida por `tcp_pcbs_lock`.
    *   Máquina de estados TCP RFC 793 (estados `CLOSED`, `LISTEN`, `SYN_SENT`, `SYN_RECEIVED`, `ESTABLISHED`).
    *   Checksum RFC 793 com pseudo-cabeçalho IPv4.
    *   Camada de sockets vinculada a nós de dispositivo do VFS para `sys_socket`, `sys_bind`, `sys_sendto` e `sys_recvfrom`.

---

## 11. Estado da Persistência

*   **Validação Dual-Boot:**
    *   **Boot 1:** Criação do diretório `/persist` e gravação do arquivo `/persist/photon.txt` contendo carga útil e sincronização forçada com o disco ATA via `sync`.
    *   **Reboot Completo:** Encerramento e reinicialização fria da máquina virtual QEMU.
    *   **Boot 2:** Montagem automática do volume FAT16 e leitura de `/persist/photon.txt`.
*   **Resultado:** Conteúdo, tamanho e metadados lidos com 100% de exatidão (`PERSISTENCE REBOOT TEST PASSED 100%`).

---

## 12. Auditoria de Locks e Sincronização

| Lock | Tipo | Recurso Protegido | Contexto de IRQ | SMP Safe | Risco de Deadlock |
| :--- | :--- | :--- | :--- | :--- | :--- |
| `task_table_lock` | Spinlock | Tabela de tarefas, filas do escalonador, troca de contexto | `spin_lock_irqsave` | Sim | Nenhum (nível base) |
| `vmm_lock` | Spinlock | Tabelas de páginas PML4/PDPT/PD/PT, TLB shootdown | `spin_lock_irqsave` | Sim | Nenhum |
| `heap_lock` | Spinlock | Headers do kmalloc/kfree, lista de blocos livres | `spin_lock_irqsave` | Sim | Nenhum |
| `video_lock` | Spinlock | Backbuffer do Framebuffer, cursor VGA | `spin_lock_irqsave` | Sim | Nenhum |
| `serial_lock` | Spinlock | Registradores de I/O da UART COM1 | `spin_lock_irqsave` | Sim | Nenhum |
| `smp_lock` | Spinlock | Sincronização de boot dos APs, contagem de CPUs | `spin_lock` | Sim | Nenhum |
| `vfs_mutex` | Mutex | Árvore de nós VFS, lista de montagens, links | Thread/Processo | Sim | Nenhum (ordem global 1) |
| `fat16_mutex` | Mutex | Tabelas de diretórios FAT16, tabela de alocação FAT | Thread/Processo | Sim | Nenhum (ordem global 2) |
| `ext2_mutex` | Mutex | Superbloco ext2, descritores de grupo, bitmaps | Thread/Processo | Sim | Nenhum (ordem global 2) |
| `bcache_mutex` | Mutex | Tabela hash do block cache, buffers dirty | Thread/Processo | Sim | Nenhum (ordem global 3) |
| `ata_mutex` | Mutex | Registradores de portas do barramento ATA/IDE | Thread/Processo | Sim | Nenhum (ordem global 4) |
| `sockets_mutex`| Mutex | Tabela de descritores de sockets | Thread/Processo | Sim | Nenhum |
| `tcp_pcbs_lock`| Mutex | Lista global de PCBs TCP | Thread/Processo | Sim | Nenhum |
| `pipe->lock` | Mutex | Buffer circular de dados do pipe | Thread/Processo | Sim | Nenhum |

**Hierarquia Global de Aquisição de Locks:**
`vfs_mutex` ➔ `fat16_mutex` / `ext2_mutex` ➔ `bcache_mutex` ➔ `ata_mutex`

---

## 13. Regressões e Falhas Encontradas na Auditoria

1. **Colisão de Endereço do Trampolim de Sinais:**
   *   *Sintoma:* Binário do shell ELF tinha sua seção `.rodata` zerada durante o carregamento inicial.
   *   *Causa:* `SIGNAL_TRAMPOLINE_ADDR` estava em `0x8000008000`, colidindo com a segunda página de dados do binário de usuário.
   *   *Resolução:* Realocado para `0x0000008000700000ULL` com elevação de `ELF_USER_STACK_TOP` para `0x0000008000800000ULL`.
2. **Nomes de Arquivo em Testes fora do Padrão FAT16 8.3:**
   *   *Sintoma:* Falhas esporádicas de criação de arquivos com nomes > 8 caracteres no FAT16.
   *   *Resolução:* Nomes ajustados para conformidade 8.3 (`synctest.txt`, `locktest.txt`, `fcntltst.txt`, `umaskdir`).
3. **Limite de Setores do Kernel:**
   *   *Sintoma:* Risco de truncamento de binário do kernel pelo bootloader Stage 1.
   *   *Resolução:* `KERNEL_SECTORS` aumentado de 416 para 480 setores (240 KiB) no `boot.asm` e `Makefile`.

---

## 14. Checklist Final de Auditoria

| Item | Classificação | Evidência Comprovada |
| :--- | :--- | :--- |
| **Boot x86_64 Long Mode** | **PASS** | 10/10 boots no QEMU, GDT/TSS 64-bit ativos |
| **IDT inicializada no início do kernel** | **PASS** | `BOOT: IDT READY` executado na primeira instrução de `kmain()` |
| **PMM** | **PASS** | Alocação/desalocação de frames físicos com contagem de referências |
| **VMM** | **PASS** | Paginação 4 níveis, DPM e mapeamento recursivo funcionais |
| **WP (Write Protect)** | **PASS** | CR0.WP ativo impedindo gravações indevidas do Ring 0 |
| **NX / W^X** | **PASS** | EFER.NXE ativo e páginas de dados com `PAGE_NX` |
| **Framebuffer** | **PASS** | LFB mapeado e limpo em `0xFD000000` |
| **Console VGA** | **PASS** | Driver de vídeo com exclusão mútua e fonte 8x16 |
| **SMP 4 CPUs** | **PASS** | 4 CPUs ativas recebendo e confirmando IPIs |
| **APIC / IOAPIC** | **PASS** | LAPIC timer ativo por CPU, PIC 8259 desativado |
| **TCP / Ping** | **PASS** | Suite TCP Fase 1 OK; Ping com 4/4 pacotes recebidos (0% perda) |
| **Shell Ring 3** | **PASS** | Shell ELF interativo executando em modo usuário |
| **ulibc** | **PASS** | Biblioteca padrão Ring 3 compilada e vinculada aos utilitários |
| **VFS** | **PASS** | Árvore de nós, resolução de caminhos e despachos genéricos OK |
| **vfs_dir_entry_t ABI** | **PASS** | Estrutura de entradas de diretório compatível |
| **ref_count** | **PASS** | Ciclo de vida de nós e descritores protegido contra UAF |
| **unlink persistente FAT16** | **PASS** | Remoção de entradas e liberação de clusters no FAT16 |
| **limpeza de código morto FAT16**| **PASS** | Código inalcançável expurgado |
| **validação VMM_USER_BASE** | **PASS** | Ponteiros de usuário validados a partir de `0x1000` |
| **Guard Page** | **PASS** | Páginas não-presentes na base das pilhas de kernel |
| **roteamento Timer APIC** | **PASS** | Interrupções de timer direcionadas sem conflitos de vetor |
| **chdir** | **PASS** | Alteração de diretório de trabalho corrente validada |
| **getcwd** | **PASS** | Consulta de diretório de trabalho corrente validada |
| **truncate** | **PASS** | Truncamento e expansão de arquivo por caminho OK |
| **ftruncate** | **PASS** | Truncamento e expansão de arquivo por fd aberto OK |
| **file_description_t** | **PASS** | Descrição de arquivo compartilhada entre dup e fork |
| **dup** | **PASS** | Duplicação de descritores com offset compartilhado |
| **dup2** | **PASS** | Duplicação atômica sobre descritor alvo |
| **fork** | **PASS** | Criação de processo filho com clonagem de memória e FDs |
| **Serial COM1 SMP-safe** | **PASS** | UART protegida por `serial_lock` nos 4 núcleos |
| **Block Cache** | **PASS** | Camada unificada de 64 blocos com cache hash e LRU |
| **sync** | **PASS** | Descarga e sincronização forçada de blocos sujos no disco |
| **umask** | **PASS** | Mascaramento de permissões aplicado em arquivos e pastas |
| **permissões POSIX** | **PASS** | Atributos `mode_t`, `uid` e `gid` validados |
| **fcntl** | **PASS** | Operações `F_DUPFD`, `F_GETFL`, `F_SETFL` funcionais |
| **flock** | **PASS** | Travas de arquivo `LOCK_SH`, `LOCK_EX` e `LOCK_UN` funcionais |
| **SIGNAL_TRAMPOLINE_ADDR** | **PASS** | Trampolim isolado em `0x0000008000700000ULL` |
| **KERNEL_SECTORS** | **PASS** | 480 setores configurados com 30 KiB de margem livre |
| **vfstest** | **PASS** | Suite completa VFS Ring 3 com 100% de aprovação |
| **persistência após reboot** | **PASS** | Dados preservados e validados no Boot 2 |
| **ext2 dynamic mount** | **PASS** | Função `ext2_mount_at` integrada ao VFS |
| **mount** | **PASS** | Comando e syscall de montagem de volumes OK |
| **umount** | **PASS** | Comando e syscall de desmontagem de volumes OK |
| **pipelines** | **PASS** | Encadeamento `cmd1 | cmd2` via pipe anônimo OK |
| **redirecionamento >** | **PASS** | Redirecionamento de stdout para arquivo OK |
| **redirecionamento <** | **PASS** | Redirecionamento de stdin a partir de arquivo OK |
| **cat via stdin** | **PASS** | Comando `cat` lendo de fd 0 funcional |
| **smptest** | **PASS** | Teste de estresse massivo de I/O concorrente em 4 CPUs OK |
| **testes automatizados** | **PASS** | Suíte `test_phase7.py` com 14/14 verificações PASS |
| **persistência automatizada** | **PASS** | Suíte `run_persistence_verification.py` 100% PASS |

---

## 15. Roadmap e Preparação Técnica para o PhotonOS v4.3

Com a fundação do **PhotonOS v4.2.1** auditada, estabilizada e comprovada em 100% dos testes, a versão **v4.3** focará nas seguintes frentes arquiteturais:

1. **Processos e IPC Avançado:**
   *   Sinais POSIX completos (`SIGCHLD`, `SIGPIPE`, `SIGCONT`, `SIGSTOP`).
   *   Sessões e grupos de processos (`setsid`, `setpgid`, `getpgrp`).
   *   Comunicação por memória compartilhada e semáforos POSIX.
2. **Pilha de Rede e TCP em User Space:**
   *   Evolução da máquina de estados TCP para transmissão e recepção de fluxos de dados bidirecionais contínuos (Fase 2).
   *   Gerenciamento de janelas deslizantes (Sliding Window) e controle de congestionamento.
   *   Biblioteca de sockets em Ring 3 para servidores concorrentes (`listen`, `accept`, `recv`, `send`).
3. **Aplicações e Serviços de Rede em Ring 3:**
   *   Servidor TCP Echo de alto desempenho em modo usuário.
   *   Servidor HTTP/1.1 estático servindo páginas diretamente do VFS (FAT16/ext2).

---

## 16. Conclusão

O **PhotonOS v4.2.1** encontra-se em estado **estável, robusto e sem regressões conhecidas**, pronto para o início do ciclo de desenvolvimento da **v4.3**.
