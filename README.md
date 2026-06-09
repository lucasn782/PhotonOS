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
* Integração com Virtual File System (VFS).
* Montagem de volumes FAT16.
* Persistência de dados em disco virtual.
* Infraestrutura preparada para expansão de operações de leitura e escrita.

### Rede

* Detecção de dispositivos PCI.
* Driver Intel e1000.
* Inicialização MMIO.
* Transmissão e recepção de quadros Ethernet.
* Infraestrutura de rede acessível por aplicações de usuário.
* Aplicativo `ping` executando em Ring 3.

### Console e Utilitários

* Shell interativo.
* Biblioteca padrão de usuário (`ulibc`).
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
| FAT16            | ✅ Operacional  |
| VFS              | 🟡 Em expansão |
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
-drive format=raw,file=disk.img,if=ide,index=0,media=disk ^
-netdev user,id=net0 ^
-device e1000,netdev=net0
```

### Modo Headless

```bash
qemu-system-x86_64 ^
-drive format=raw,file=build/photon.img,if=floppy ^
-drive format=raw,file=disk.img,if=ide,index=0,media=disk ^
-netdev user,id=net0 ^
-device e1000,netdev=net0 ^
-nographic
```

---

## 📈 Próximos Objetivos

### Sistema de Arquivos

* Navegação por subdiretórios FAT16.
* Expansão do VFS.
* Operações avançadas de escrita.
* Criação dinâmica de arquivos.

### Espaço de Usuário

* Expansão da interface de syscalls.
* Execução dinâmica de programas.
* Gerenciamento avançado de processos.

### Rede

* Abstração de sockets.
* Camada IP completa.
* Suporte ampliado a protocolos.

### Kernel

* Aprimoramento da proteção de memória.
* Ferramentas de depuração do scheduler.
* Otimização da troca de contexto.

---

## 🎯 Objetivo do Projeto

O PhotonOS é um projeto educacional e experimental voltado ao estudo aprofundado de sistemas operacionais modernos, explorando desde a inicialização em Assembly até a execução de aplicações em espaço de usuário, passando por gerenciamento de memória, armazenamento persistente e comunicação em rede.
