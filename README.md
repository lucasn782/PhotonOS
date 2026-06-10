# PhotonOS 🌌

O **PhotonOS** é um sistema operacional monolítico freestanding desenvolvido do zero para a arquitetura **x86_64**, executando em **64-bit Long Mode**. O projeto tem como objetivo explorar os principais conceitos de engenharia de sistemas operacionais modernos, incluindo gerenciamento de memória, multitarefa preemptiva, isolamento de privilégios, sistemas de arquivos persistentes e comunicação em rede.

Atualmente o sistema possui suporte a execução em Ring 3, carregamento de binários ELF, escalonamento multitarefa, armazenamento FAT16, driver PCI Intel e1000 e uma interface Shell própria executando em espaço de usuário.

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

### Inicialização e Arquitetura

* Bootloader próprio em Assembly x86.
* Transição de Modo Real (16-bit) para Modo Protegido (32-bit).
* Entrada em **64-bit Long Mode**.
* Kernel executando em arquitetura x86_64 freestanding.
* Paginação baseada em tabelas PML4.

### Gerenciamento de Memória

* Gerenciador de memória física (PMM).
* Gerenciamento de memória virtual (VMM).
* Kernel Heap dinâmico.
* Interface de alocação dinâmica via `kmalloc()` e `kfree()`.
* Mapeamento de memória para processos em espaço de usuário.

### Escalonamento e Concorrência

* Escalonador Round-Robin preemptivo.
* Troca de contexto baseada em interrupções do PIT.
* Controle de tarefas do Kernel.
* Estruturas de escalonamento separadas do subsistema de memória.
* Mecanismos de sincronização via Mutex.

### Espaço de Usuário (Ring 3)

* Isolamento entre Ring 0 e Ring 3.
* Troca de privilégio utilizando TSS.
* Syscalls implementadas através de `syscall/sysret`.
* Carregamento de executáveis ELF de 64 bits.
* Execução de aplicações independentes em espaço de usuário.

### Sistema de Arquivos

* Driver ATA PIO.
* Sistema de Arquivos FAT16.
* Integração com Virtual File System (VFS) com suporte completo a subdiretórios recursivos e navegação de caminhos.
* Operações de leitura (`sys_read`) e escrita (`sys_write`) persistentes através de cadeias de clusters (cluster chains).
* Alocação dinâmica de novos clusters na tabela FAT quando arquivos são expandidos.
* Sincronização automática de metadados (tamanho e cluster inicial) nas entradas de diretórios de 32 bytes no disco virtual.

### Segurança e Robustez (Ring 0 / Ring 3)

* Isolamento entre Ring 0 e Ring 3 com TSS.
* Syscalls implementadas através de `syscall/sysret`.
* Hardening de chamadas de sistema de I/O (`sys_read`, `sys_write`, `sys_execve`):
  * Validação rigorosa dos buffers do espaço de usuário (Ring 3) usando `vmm_is_mapped` antes do acesso em Ring 0.
  * Cópia de segurança de caminhos e dados via buffers temporários no Kernel Heap (`kmalloc`) para evitar page faults e vulnerabilidades de concorrência.
  * Limites rígidos de tamanho por bloco de escrita para resguardar a integridade do heap.
* Restrição absoluta de formatação: proibição do uso de especificadores de formato (`%s`, `%d`, `%x`) no logger interno (`klog`) do Ring 0.

### Compilação e Otimização do Linker

* Otimização de empacotamento com a flag `-N` (`--nmagic`) no Makefile para os executáveis do initrd.
* Fusão de seções text/data e desativação de padding de 4KB por página nos ELFs integrados, reduzindo o tamanho de `photon.bin` de **149KB** para **96KB** (respeitando o limite físico estrito de 144KB do boot loader).

### Console e Utilitários

* Shell interativo no espaço de usuário (/bin/shell) com comandos como `ls`, `ps`, `cat`, `touch`, `write` (escrita direta em arquivos), redirecionamentos (`>`) e suporte a pipes básicos.
* Biblioteca padrão de usuário (`pulibc`).
* Programas de teste e demonstração:

  * hello
  * upper
  * rev
  * spin
  * hang
  * ping

---

## 📊 Estado Atual do Desenvolvimento

| Subsistema       | Status         |
| ---------------- | -------------- |
| Bootloader       | ✅ Estável      |
| Long Mode x86_64 | ✅ Estável      |
| PMM              | ✅ Concluído    |
| VMM              | ✅ Concluído    |
| Heap do Kernel   | ✅ Concluído    |
| Scheduler        | ✅ Estável      |
| Ring 3           | ✅ Operacional  |
| Syscalls         | ✅ Operacional  |
| Loader ELF       | ✅ Operacional  |
| ATA PIO          | ✅ Operacional  |
| FAT16            | ✅ Estável      |
| VFS              | ✅ Estável      |
| PCI              | ✅ Operacional  |
| Intel e1000      | ✅ Operacional  |
| Shell            | ✅ Operacional  |
| Rede de Usuário  | 🟡 Evoluindo   |

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

* Abstração completa de sockets POSIX-like.
* Implementação do protocolo TCP no espaço de usuário.
* Desenvolvimento de um servidor HTTP simples rodando em Ring 3.

### Kernel & Sincronização

* Aprimoramento da proteção de memória ativa (flags WP, W^X) e isolamento avançado de páginas de kernel.
* Adição de semáforos e variáveis de condição ao scheduler.

---

## 🎯 Objetivo do Projeto

O PhotonOS é um projeto educacional e experimental voltado ao estudo aprofundado de sistemas operacionais modernos, explorando desde a inicialização em Assembly até a execução de aplicações em espaço de usuário, passando por gerenciamento de memória, armazenamento persistente e comunicação em rede.
