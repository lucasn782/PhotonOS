# PhotonOS v4.0 🌌

O **PhotonOS v4.0** é um sistema operacional monolítico freestanding desenvolvido do zero para a arquitetura **x86_64**, executando em **64-bit Long Mode**. O projeto concluiu com sucesso a implementação do **Sistema de Ficheiros Gravável de Alta Performance (EXT2 Nativo)** na Trilha 10, consolidando um núcleo multi-core resiliente com Copy-On-Write, pipeline gráfico por software, barramento PCI, interface de rede e1000 estável, armazenamento persistente dual (FAT16 + EXT2) e console interativo no espaço de usuário.

### 🧱 Marco Histórico de Persistência — PhotonOS v4.0
O marco v4.0 fecha a trilha de armazenamento com uma pilha de persistência de dados totalmente operacional no kernel Ring 0: o driver ATA/IDE foi blindado por `ata_mutex` para eliminar race conditions sobre os registradores físicos das portas `0x1F0`–`0x1F7`, enquanto o subsistema EXT2 passou a suportar parser do superbloco, validação do mágico `0xEF53`, carregamento da BGDT em RAM, conversão matemática de inodes via `ext2_read_inode()`/`ext2_write_inode()`, lookup recursivo de caminhos via VFS e alocação atômica de blocos e inodes com divisão de entradas de diretório.

---

## 🛠️ Estrutura do Projeto

```text
PhotonOS/
├── build/             # Binários gerados durante o processo de build
├── docs/              # Documentação técnica e diagramas
├── include/           # Headers do Kernel e drivers
├── logs/              # Logs de execução e depuração
├── references/        # Materiais de referência do projeto
├── scripts/           # Scripts auxiliares
├── src/
│   ├── boot/          # Bootloader e entrada do Kernel
│   ├── drivers/       # Drivers ATA, PCI, FAT16, e1000 e Serial
│   ├── fs/            # Sistemas de ficheiros nativos (EXT2)
│   ├── kernel/        # Núcleo do sistema
│   └── user/          # Biblioteca de usuário e aplicações
├── Makefile
└── README.md
```

---

## 🚀 Funcionalidades Consolidadas

### 📦 Sistema de Ficheiros EXT2 Nativo Gravável — V4.0 [NOVO]
> [!IMPORTANT]
> **EXT2 Writable Filesystem:** O PhotonOS v4.0 implementa suporte nativo gravável de alta performance ao **Second Extended Filesystem (EXT2)** em Ring 0, integrado ao Virtual File System (VFS) e operando via driver IDE/ATA PIO com blindagem concorrente SMP.
* **Blindagem Concorrente no Driver ATA (`ata_mutex`):** Introdução de exclusão mútua (`mutex_t`) no driver de disco `src/drivers/ata.c`, protegendo o acesso aos registradores físicos de comando e controle IDE (Portas `0x1F0`–`0x1F7`) contra race conditions induzidas por múltiplos núcleos SMP ativos. A sequência atômica *setup de registradores → emissão de comando (`0x20`/`0x30`) → transferência via `insw`/`outsw` → cache flush (`0xE7`)* é indivisível.
* **Parser de Superbloco e Validação de Integridade:** Leitura do Superbloco EXT2 no offset fixo de 1024 bytes (setores LBA+2/LBA+3) com validação estrita do número mágico `0xEF53` no campo `s_magic`. Derivação automática do tamanho de bloco via `1024 << s_log_block_size` e cálculo do número de grupos de blocos.
* **Tabela de Descritores de Grupos de Blocos (BGDT):** Carregamento integral em RAM da BGDT (array de `struct ext2_group_desc`, 32 bytes/entrada) para localização O(1) dos bitmaps de alocação e tabelas de inodes de qualquer grupo do disco.
* **Matemática de Inodes (`ext2_read_inode` / `ext2_write_inode`):** Conversão de índices lógicos de inodes para offsets de setor via aritmética modular: `group = (N-1) / s_inodes_per_group`, `index = (N-1) % s_inodes_per_group`. Operações de escrita utilizam protocolo Read-Modify-Write em nível de bloco.
* **Resolução Recursiva de Caminhos e Diretórios:** Navegação da árvore de diretórios a partir do Inode Raiz (Inode 2), interpretando blocos de dados como arrays dinâmicos de `struct ext2_dir_entry_2` com travessia via `rec_len`. Integração transparente ao VFS via ponteiros de função (`read`, `write`, `readdir`, `open`).
* **Alocação Atômica de Blocos e Inodes:** Motores de varredura bit-a-bit nos bitmaps de blocos e inodes protegidos por `ext2_mutex`, com persistência síncrona imediata dos descritores de grupo e Superbloco após cada alocação.
* **Pipeline de Escrita com Ponteiros Diretos e Indiretos:** Suporte a ficheiros extensos via 12 ponteiros diretos (`i_block[0..11]`) e 1 ponteiro simplesmente indireto (`i_block[12]`), com alocação dinâmica do bloco indireto sob demanda.
* **Divisão de Entradas de Diretório:** Algoritmo de *directory entry splitting* para inserção eficiente de novos nós no disco, reutilizando trailing space de entradas existentes ou alocando novos blocos de diretório conforme necessário.

### 🧠 Otimização de Memória: Copy-On-Write (COW) — V3.1
> [!NOTE]
> **Copy-On-Write para `sys_fork`:** O PhotonOS v3.1 implementa o mecanismo de COW no Gerenciador de Memória Virtual, eliminando a duplicação física imediata de frames de memória durante o `fork`. Pai e filho compartilham os mesmos frames físicos de 4 KiB até que uma escrita efetiva ocorra, disparando a separação preguiçosa de páginas via exceção de Page Fault (`INT 0x0E`).
* **Contador de Referências no PMM (`pmm_refcounts`):** Array estático de `uint32_t` indexado pelo número de frame físico (PFN), rastreando o grau de compartilhamento dos 32.768 frames de 4 KiB. `pmm_alloc` inicializa o contador em `1`; `pmm_free` decrementa e só devolve o frame ao pool livre quando o contador atinge zero.
* **Clonagem Preguiçosa de Páginas (`vmm_clone_address_space`):** Durante o `fork`, em vez de alocar e copiar cada frame de usuário, o kernel modifica as PTEs do pai e cria as PTEs do filho apontando para o **mesmo frame físico**: remove `PAGE_WRITABLE` (bit 1) e seta `PAGE_COW` (bit 9 = `0x200`) em ambas as entradas. Nenhuma alocação de memória extra ocorre.
* **Tratador de Falha de Página COW (`vmm_page_fault_handler`):** Intercepta exceções `#PF` (`INT 0x0E`) causadas por escrita em páginas COW. Valida o código de erro (bit 1 setado) e o bit `PAGE_COW` na PTE. Se `refcount > 1`: aloca novo frame, copia 4 KiB, atualiza a PTE com `PAGE_WRITABLE` e decrementa o refcount do frame antigo. Se `refcount == 1`: restaura `PAGE_WRITABLE` in-place sem alocação extra.
* **Consistência Multicore — TLB Shootdown via LAPIC (`Vector 0x79`):** Após a modificação das PTEs do pai no fork, o kernel verifica se há APs ativos (`smp_ap_booted_count()`) e emite um IPI broadcast via ICR do LAPIC. O handler `smp_tlb_shootdown_handler` em cada AP recarrega o CR3 para flush completo do TLB local e emite EOI.
* **Invariante de Kernel:** Páginas do Higher-Half (entradas 256–511 da PML4) e da identity map de boot (entrada 0) jamais são submetidas ao protocolo COW — somente o espaço de usuário (entradas 1–255) é afetado.

### ⚡ Multiprocessamento Simétrico (SMP) - V3.0
> [!NOTE]
> **Arquitetura Multi-Core Nativa:** O PhotonOS v3.0 oferece suporte a multiprocessamento simétrico na arquitetura x86_64. O Bootstrap Processor (BSP) gerencia a descoberta e a inicialização física individual dos Application Processors (APs), possibilitando a execução paralela em Ring 0.
* **Ecossistema APIC Nativo:** Desativação completa do controlador PIC 8259 legado (mascarando todas as linhas de interrupção nas portas de I/O `0x21` e `0xA1`) e ativação do Local APIC (LAPIC) para permitir o envio de interrupções interprocessador (IPIs) e suporte futuro ao I/O APIC.
* **Código Trampolim Alocado em `0x7000`:** Copiado dinamicamente como um blob binário de 16 bits para o endereço físico `0x7000` (garantido pelo mapeamento de identidade), servindo como ponto de entrada em Modo Real e guiando os APs na transição segura para o Modo Longo de 64 bits.
* **Sequenciamento INIT-SIPI Calibrado por `rdtsc`:** Utilização de atrasos de temporização calibrados pelo registrador de timestamp (`rdtsc`) para o envio seguro de IPIs de inicialização (INIT) e de inicialização física (Startup IPI) através do registrador ICR do LAPIC.
* **Pilhas Isoladas por Núcleo:** Alocação dinâmica de páginas físicas individuais de 4 KiB pelo PMM para funcionarem como pilhas de kernel Ring 0 exclusivas para cada processador secundário, evitando corrupções por colisão de memória.
* **Sincronização por Spinlocks Atômicos:** Implementação de primitivas de exclusão mútua baseadas em Spinlocks de barramento bloqueante (`__sync_lock_test_and_set`), garantindo sincronização e serialização de logs durante a execução da função `ap_kmain` nos processadores secundários.

### Inicialização, Modo Longo de 64-bits & Core
* **GDT e Alinhamento:** Correção do descritor da GDT (`dq` substituindo o antigo `dd`), garantindo a carga linear correta dos 64 bits da base e impedindo falhas de paginação silenciosas.
* **TSS e Mitigação de Falhas:** Configuração do Interrupt Stack Table (IST1) na TSS apontando para pilha isolada dedicada (`double_fault_stack`), com a flag `idt[8].ist = 1` configurada, impedindo Triple Faults na ocorrência de estouros de pilha de kernel.
* Bootloader próprio em Assembly x86.
* Transição de Modo Real (16-bit) para Modo Protegido (32-bit) e entrada em **64-bit Long Mode**.
* Paginação baseada em tabelas PML4.

### Gerenciamento de Memória
* Gerenciador de memória física (PMM) e de memória virtual (VMM).
* Kernel Heap dinâmico com interface de alocação via `kmalloc()` e `kfree()`.
* Mapeamento de memória para processos em espaço de usuário.

### Subsistema Gráfico, Terminal e UI por Software
* **Pipeline Gráfico:** Ativação do modo VBE em 1024x768x32-bit com mapeamento de alta memória para o Linear Framebuffer (LFB) em `0xFFFFFFFFC0000000` (flags: Cache Disable e Write-Through).
* **Console Dinâmico:** Substituição de constantes VGA estáticas por uma grade adaptativa orientada a Stride ($128 \times 48$). Implementação de *Deferred Buffering* no backbuffer mapeado em `0xFFFFFFFFC1000000` para otimização de barramento.
* **Elementos de UI:** Cursor de texto renderizado via software e sprite do mouse em formato de Seta Angular Simétrica ($19 \times 12$), com salvamento e restauração dos pixels subjacentes no backbuffer para evitar rastros.

### Escalonamento, Concorrência e IPC
* **Atomicidade em Pipes:** Blindagem de segurança nas rotinas `pipe_read` e `pipe_write` usando exclusão mútua (`pipe->lock` via mutexes) e controle estrito de interrupções, eliminando janelas de vulnerabilidade assíncronas e prevenindo Deadlocks durante o encadeamento de comandos no `/bin/shell`.
* Escalonador Round-Robin preemptivo baseado em interrupções do PIT.
* Estruturas de escalonamento isoladas do subsistema de memória e mecanismos de sincronização via Mutex.

### Espaço de Usuário (Ring 3)
* Isolamento entre Ring 0 e Ring 3 com troca de privilégio utilizando TSS.
* Syscalls implementadas através de `syscall/sysret` com a macro `SYS_EXIT` mapeada corretamente no ID 5.
* Carregamento de executáveis ELF de 64 bits.
* **Gerenciamento Dinâmico de Processos**: Suporte nativo a `sys_fork` (Syscall 23) e estabelecimento da trindade POSIX de processos (Fork, Exec, Exit).

### Sistema de Arquivos
* Driver ATA PIO com blindagem de concorrência SMP (`ata_mutex`) e Sistemas de Arquivos FAT16 e **EXT2 Nativo Gravável**.
* Integração com Virtual File System (VFS) com suporte completo a subdiretórios recursivos, navegação de caminhos e detecção automática de tipo de sistema de ficheiros (FAT16 → EXT2 fallback).
* Operações de leitura (`sys_read`) e escrita (`sys_write`) persistentes através de alocação dinâmica de clusters (FAT16) e blocos com ponteiros diretos/indiretos (EXT2).
* Parser de Superbloco EXT2 com validação de integridade (`0xEF53`), alocação atômica de inodes e blocos via bitmaps protegidos por mutex, e algoritmo de divisão de entradas de diretório.

### Segurança e Robustez (Ring 0 / Ring 3)
* Hardening de chamadas de sistema de I/O (`sys_read`, `sys_write`, `sys_execve`) com validação de buffers do espaço de usuário (`vmm_is_mapped`) e cópia de segurança em buffers do kernel (`kmalloc`).
* Restrição absoluta de formatação: proibição do uso de especificadores de formato (`%s`, `%d`, `%x`) no logger interno (`klog`) do Ring 0.

### Subsistema de Rede & Barramento PCI
* **Sondagem de Hardware:** Varredura dinâmica de barramento PCI baseada em Class Code (`0x02`) e Subclass Code (`0x00`) para detecção de placas Ethernet via portas `0xCF8`/`0xCFC`.
* **DMA Físico Puro:** Alocação e alinhamento do anel de descritores RX/TX e buffers de pacotes usando diretamente frames físicos provenientes do PMM (`pmm_alloc`), mapeados no espaço virtual do driver e1000, ativando o recurso de *Bus Mastering* e possibilitando a operação robusta do utilitário `ping` nativo.
* **Sockets POSIX:** Chamadas de sistema `sys_socket` (ID 24) e `sys_bind` (ID 25) integradas ao VFS com proteção atômica de ring buffers e I/O não-bloqueante (`-EAGAIN`).
* **Checksums:** Validação de checksums IP, ICMP e UDP (com pseudo-cabeçalho) em conformidade com a RFC 768.

### Compilação e Otimização do Linker
* Otimização de empacotamento com a flag `-N` (`--nmagic`) no Makefile para os executáveis do initrd, reduzindo o tamanho de `photon.bin` para respeitar o limite de 144KB.

### Console e Utilitários
* Shell interativo no espaço de usuário (/bin/shell) com comandos como `ls`, `ps`, `cat`, `touch`, `write`, redirecionamentos (`>`) e suporte a pipes.
* Biblioteca padrão de usuário (`pulibc`).
* Programas de teste e demonstração:
  * hello
  * upper
  * rev
  * spin
  * hang
  * ping

---

## 📊 Status do Projeto & Ecossistema de Trilhas

O desenvolvimento do PhotonOS é estruturado em trilhas de aprendizado e implementação de engenharia de software de baixo nível. Com a conclusão do subsistema de armazenamento EXT2 gravável, todas as dez trilhas principais do sistema estão concluídas.

**Progresso Geral do Sistema:**
`[██████████████████████████████████████████████████]` **100% Concluído (V4.0)**

### 🛣️ Ecossistema de Trilhas de Engenharia

| Trilha | Descrição | Status | Versão de Entrega |
| :---: | :--- | :---: | :---: |
| **Trilha 1** | Bootloader Real Mode (16-bit) e Transição de Privilégios | 100% | v1.0 |
| **Trilha 2** | GDT, IDT, Tratamento de Interrupções e Proteção de Ring 0 | 100% | v1.0 |
| **Trilha 3** | Configuração de Paginação (PML4) e Entrada em 64-bit Long Mode | 100% | v1.0 |
| **Trilha 4** | Gerenciador de Memória Física (PMM), Virtual (VMM) e Heap Dinâmico | 100% | v1.0 |
| **Trilha 5** | Processos, Escalonamento Preemptivo (Round-Robin) e Espaço de Usuário | 100% | v2.0 |
| **Trilha 6** | Armazenamento (ATA PIO), Sistema de Arquivos FAT16 e VFS POSIX | 100% | v2.0 |
| **Trilha 7** | Subsistema Gráfico VBE, Double Buffering e Barramento PCI/Driver e1000 | 100% | v2.0 |
| **Trilha 8** | Multiprocessamento Simétrico (SMP): Suporte Multi-Core Nativo | 100% | v3.0 |
| **Trilha 9** | Otimização de Memória Virtual: Copy-On-Write (COW) para `sys_fork` | 100% | v3.1 |
| **Trilha 10** | **Sistema de Ficheiros EXT2 Nativo Gravável e Blindagem Concorrente ATA** | **100%** | **v4.0** |

---

## 🔧 Compilação

### Dependências

* GCC Cross Compiler (x86_64-elf-gcc)
* Binutils (x86_64-elf-ld)
* NASM
* Make
* QEMU
* WSL (Windows Subsystem for Linux) — recomendado para ambiente Windows

### Build Unificado (WSL)

O comando unificado abaixo realiza a limpeza completa do ambiente de build, compila todos os componentes do kernel e gera a imagem de disco FAT16:

```bash
make clean; make; make fat16-disk
```

> **Nota WSL**: Em ambientes Windows, utilize ponto e vírgula (`;`) como separador de comandos no PowerShell. Para shells POSIX (bash/zsh no WSL), `&&` também é aceito.

---

## ▶️ Execução no QEMU

### Modo Gráfico

```bash
qemu-system-x86_64 ^
-drive format=raw,file=build/photon.img,if=floppy ^
-drive format=raw,file=build/disk.img,if=ide,index=0,media=disk ^
-netdev user,id=net0 ^
-device e1000,netdev=net0
```

### Modo Headless

```bash
qemu-system-x86_64 ^
-drive format=raw,file=build/photon.img,if=floppy ^
-drive format=raw,file=build/disk.img,if=ide,index=0,media=disk ^
-netdev user,id=net0 ^
-device e1000,netdev=net0 ^
-nographic
```

---

## 📈 Próximos Objetivos

### Sistema de Arquivos & VFS

* Implementação de permissões básicas de arquivos no VFS.
* Mapeamento de links simbólicos e montagem dinâmica de múltiplos volumes.

### Espaço de Usuário & Rede

* Implementação do protocolo TCP no espaço de usuário.
* Desenvolvimento de um servidor HTTP simples rodando em Ring 3.

### Kernel & Sincronização

* Aprimoramento da proteção de memória ativa (flags WP, W^X) e isolamento avançado de páginas de kernel.
* Adição de semáforos e variáveis de condição ao scheduler.

---

## 🕒 Changelog / Linha do Tempo

### `v4.0` - The EXT2 Persistent Storage Update 📦 (Versão Atual)
* **Sistema de Ficheiros EXT2 Nativo Gravável:** Implementação completa do driver EXT2 em Ring 0 (`src/fs/ext2.c`, `include/fs/ext2.h`) com suporte a leitura, escrita, criação de ficheiros e listagem de diretórios.
* **Blindagem Concorrente no Driver ATA (`ata_mutex`):** Introdução de exclusão mútua no driver IDE/ATA protegendo a sequência atômica de acesso aos registradores de I/O `0x1F0`–`0x1F7` contra race conditions SMP.
* **Parser de Superbloco com Validação de Integridade:** Leitura e validação do número mágico `0xEF53` no offset fixo de 1024 bytes, com derivação automática do tamanho de bloco e carregamento integral da Tabela de Descritores de Grupos de Blocos em RAM.
* **Matemática de Inodes e Resolução de Caminhos:** Aritmética modular de conversão `inode_num → (grupo, índice, bloco, offset)` e navegação recursiva de diretórios a partir do Inode Raiz 2, interpretando `struct ext2_dir_entry_2` com travessia via `rec_len`.
* **Alocação Atômica de Blocos e Inodes:** Motores de varredura bit-a-bit em bitmaps de blocos e inodes protegidos por `ext2_mutex`, com persistência síncrona do Superbloco e descritores de grupo após cada alocação.
* **Pipeline de Escrita Extenso:** Suporte a ponteiros diretos (`i_block[0..11]`) e simplesmente indiretos (`i_block[12]`) com alocação dinâmica sob demanda e protocolo Read-Modify-Write para escritas parciais em blocos.
* **Algoritmo de Divisão de Entradas de Diretório:** Inserção de novos ficheiros via reutilização de trailing space, divisão de `rec_len` ou alocação de novos blocos de diretório.
* **Detecção Automática FAT16 → EXT2:** Fallback transparente em `ata_vfs_init()` — tenta FAT16 primeiro; se falhar, monta como EXT2.

### `v3.1` - The COW Memory Optimization Update 🧠
* **Copy-On-Write (COW) para `sys_fork`:** Implementação completa do mecanismo de compartilhamento preguiçoso de páginas físicas entre processos pai e filho durante a bifurcação.
* **Contador de Referências no PMM (`pmm_refcounts`):** Array estático de `uint32_t` indexando os 32.768 frames físicos de 4 KiB — integrado a `pmm_alloc` e `pmm_free` com semântica de decremento atômico.
* **Handler de Page Fault COW (`INT 0x0E`):** Registro de `page_fault_stub` na IDT (vetor 14) e implementação de `vmm_page_fault_handler` com lógica de divisão de frame (*page splitting*) baseada no refcount.
* **TLB Shootdown via LAPIC (Vector `0x79`):** Sincronização multicore via IPI broadcast após modificação de PTEs no fork, garantindo coerência do TLB em todos os núcleos AP ativos.
* **Flags de PTE Customizadas:** `PAGE_COW` (`0x200`, bit 9) definido no OS-Available field da PTE x86_64 — compatível com o Intel SDM Vol. 3A §4.5.

### `v3.0` - The SMP Update 🚀
* **Multiprocessamento Simétrico (SMP):** Suporte nativo a múltiplos núcleos de processamento (BSP + APs) com sincronização e inicialização de hardware avançada.
* **Ecossistema APIC:** Desativação do PIC 8259 legado (portas `0x21` e `0xA1`) e ativação do Local APIC (LAPIC) no BSP e APs.
* **Código Trampolim em 0x7000:** Bootstrap de processadores secundários de Modo Real de 16-bit para Modo Longo de 64-bit.
* **Sincronização Ring 0:** Spinlocks atômicos baseados em instruções intrínsecas do compilador.
* **Pilha Isolada por Núcleo:** Alocação de páginas físicas dedicadas de 4 KiB por Application Processor.

### `v2.0` - The Graphics & Networking Update 🌌
* Implementação do driver e1000 PCI de rede e sockets UDP.
* Pipeline gráfico por software via VBE (1024x768x32bpp) com Double Buffering.
* Mitigação de Double Faults e suporte à trindade fork/exec/exit em Ring 3.

### `v1.0` - The Core 64-bit Update ⚙️
* Setup básico de kernel 64-bit, tabelas PML4 de paginação, tratamento de IRQs, GDT/IDT/TSS estáveis e Heap do Kernel.

---

## 🎯 Objetivo do Projeto

O PhotonOS é um projeto educacional e experimental voltado ao estudo aprofundado de sistemas operacionais modernos, explorando desde a inicialização em Assembly até a execução de aplicações em espaço de usuário, passando por gerenciamento de memória, armazenamento persistente e comunicação em rede.
