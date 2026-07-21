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

---

## 🟡 Em Desenvolvimento (v4.2-dev)

### Subsistemas de Arquivos & Rede
- **Links Simbólicos (Symlinks):** Resolução e suporte a caminhos com symlinks no VFS.
- **Pilha TCP Stream Transmissão/Recepção:** Implementação completa da transmissão de dados confiável (VFS read/write) para sockets TCP.
- **Sincronização de Sockets e Teclado**: Proteção concorrente do ring buffer do teclado via spinlocks e polimento da fila de rede.

---

## 🔵 Planejado

### Sistema de Arquivos & VFS
- **Permissões de Arquivos:** Suporte a permissões Unix (Read/Write/Execute) no VFS.
- **Montagem Dinâmica:** Suporte à montagem de múltiplos volumes em diferentes nós do VFS.

### Rede e Usuário
- **Pilha TCP no Espaço de Usuário:** Implementação completa de transmissão de fluxo confiável com controle de fluxo.
- **Servidor HTTP:** Aplicação demonstração rodando em Ring 3 escutando conexões web.

### Kernel & Segurança
- **Proteção de Páginas de Kernel:** Ativação de bits WP (Write Protect) e NX (No-Execute) nas tabelas PML4 de kernel.
- **Sincronização Avançada:** Adição de semáforos e variáveis de condição ao escalonador para bloqueio eficiente de tarefas.

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

---

## ⚠️ Problemas Conhecidos

1. **Assinatura de ABI Inconsistente:** A mudança de `size_t count` para `int count` nas chamadas `read()` e `write()` da ulibc causa uma divergência formal com o padrão POSIX, embora não tenha impacto funcional sob os limites do sistema operacional atual.
2. **Poli-alocação de buffer gráfico sob stress:** Múltiplas atualizações sucessivas do console gráfico podem resultar em lag perceptível devido à transferência direta do backbuffer para o Linear Framebuffer sem sincronização vertical (V-Sync).
