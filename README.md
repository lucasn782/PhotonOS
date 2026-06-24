# PhotonOS v2.0 🌌

O **PhotonOS v2.0** é um sistema operacional monolítico freestanding desenvolvido do zero para a arquitetura **x86_64**, executando em **64-bit Long Mode**. O projeto concluiu com sucesso o seu ciclo de estabilização do Ring 0 e ativação de hardware, fornecendo um kernel robusto com suporte a execução em Ring 3, pipeline gráfico por software, barramento PCI, interface de rede e1000 estável, e uma interface Shell própria executando em espaço de usuário.

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
│   ├── kernel/        # Núcleo do sistema
│   └── user/          # Biblioteca de usuário e aplicações
├── Makefile
└── README.md
```

---

## 🚀 Funcionalidades Consolidadas

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
* Driver ATA PIO e Sistema de Arquivos FAT16.
* Integração com Virtual File System (VFS) com suporte completo a subdiretórios recursivos e navegação de caminhos.
* Operações de leitura (`sys_read`) e escrita (`sys_write`) persistentes através de alocação dinâmica de novos clusters na tabela FAT.

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

## 📊 Status de Desenvolvimento do Núcleo

| Subsistema                     | Status         |
| ------------------------------ | -------------- |
| Bootloader                     | ✅ Estável      |
| Long Mode x86_64               | ✅ Estável      |
| PMM / VMM / Heap               | ✅ Concluído    |
| Escalonador e Concorrência     | ✅ Concluído    |
| Espaço de Usuário e Syscalls   | ✅ Concluído    |
| Loader ELF                     | ✅ Operacional  |
| ATA PIO / FAT16 / VFS          | ✅ Estável      |
| Subsistema Gráfico e UI        | ✅ Concluído    |
| IPC e Sincronização de Pipes   | ✅ Concluído    |
| Barramento PCI & Intel e1000   | ✅ Operacional  |
| Pilha de Rede e Sockets        | ✅ Concluído    |
| Shell                          | ✅ Operacional  |

**Métrica Total do Sistema:** 100%

---

## 🔧 Compilação

### Dependências

* GCC Cross Compiler
* Binutils
* NASM
* Make
* QEMU

### Build Completo

```bash
make clean
make
```

### Geração do Disco FAT16

```bash
make fat16-disk
```

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

## 🎯 Objetivo do Projeto

O PhotonOS é um projeto educacional e experimental voltado ao estudo aprofundado de sistemas operacionais modernos, explorando desde a inicialização em Assembly até a execução de aplicações em espaço de usuário, passando por gerenciamento de memória, armazenamento persistente e comunicação em rede.
