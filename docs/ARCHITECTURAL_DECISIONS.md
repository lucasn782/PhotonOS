# 🏛️ PhotonOS — Registro de Decisões Arquiteturais (ADR)

Este documento registra de forma cronológica as decisões de design arquitetural mais significativas no desenvolvimento do kernel e da ulibc do PhotonOS.

---

## 1. Otimização de Memória Virtual: Copy-On-Write (COW)
* **Data:** 2026-06-24
* **Versão:** v3.1

### Motivação
A implementação original do `sys_fork` utilizava clonagem profunda (deep-copy) das tabelas de páginas PML4. Isso resultava na alocação física imediata de memória para todas as páginas do processo filho, gerando alto overhead de CPU e consumo excessivo de RAM para processos que executavam imediatamente a chamada `execve()`.

### Alternativas Consideradas
- **Clonagem Profunda Imediata (Deep-Copy):** Abordagem simples, porém ineficiente para recursos limitados de RAM e CPU.
- **Copy-On-Write (Adotada):** Compartilhar as páginas de memória de forma preguiçosa, duplicando-as apenas sob demanda de escrita.

### Solução Adotada
Configuração da tabela de páginas do processo filho apontando para os mesmos frames físicos do pai. As permissões de escrita (`PAGE_WRITABLE`) são removidas de ambas as tabelas e o bit 9 (disponível para o sistema operacional) é setado como `PAGE_COW` (`0x200`). Uma escrita causa uma exceção de Falha de Página (`INT 0x0E`). O handler de falha verifica o bit `PAGE_COW`, aloca um novo frame físico, copia os 4 KiB de dados, restaura a flag de escrita e decrementa o contador de referências do frame compartilhado.

### Impacto
- Redução drástica da latência de criação de processos.
- Complexidade adicional no gerenciamento de memória física (necessidade de array de referências `pmm_refcounts`).
- Necessidade de coordenar invalidações de cache de TLB em múltiplos núcleos (TLB Shootdown).

### Arquivos Envolvidos
- `src/kernel/vmm.c`
- `src/kernel/memory.c`
- `include/vmm.h`
- `include/memory.h`

---

## 2. Multiprocessamento Simétrico (SMP) e Sincronização
* **Data:** 2026-06-24
* **Versão:** v3.0

### Motivação
A transição de um sistema single-core para multi-core é necessária para usufruir de paralelismo real nas CPUs modernizadas x86_64 suportadas pelo QEMU.

### Alternativas Consideradas
- **Monoprocessador (Uniprocessor):** Limitação de performance e desperdício de recursos da CPU simulada.
- **Cooperative Multiprocessing:** Difícil implementação e baixa responsividade do espaço de usuário.
- **Multiprocessamento Simétrico (Adotada):** Execução do mesmo código de kernel em múltiplos núcleos independentes.

### Solução Adotada
Desativação do controlador PIC 8259 legado e mapeamento do Local APIC (LAPIC) como controlador principal de interrupções. O BSP copia um código trampolim de 16 bits para o endereço físico fixo `0x7000` (identity-mapped) e envia IPIs do tipo INIT-SIPI-SIPI para acordar os processadores secundários (APs). A sincronização do kernel é realizada por spinlocks atômicos implementados através das primitivas incorporadas do compilador GCC (`__sync_lock_test_and_set`).

### Impacto
- Kernel e drivers precisam ser reentrantes e thread-safe.
- Alocação isolada de pilhas de kernel e dados locais por núcleo (TSS/GDT reloaded por AP).
- Introdução de overhead sutil de disputa de barramento (bus contention) devido ao uso de spinlocks.

### Arquivos Envolvidos
- `src/kernel/smp.c`
- `src/kernel/apic.c`
- `src/kernel/trampoline.asm`
- `include/smp.h`
- `include/apic.h`

---

## 3. Blindagem de Acesso Concorrente ao Disco (Driver ATA)
* **Data:** 2026-07-01
* **Versão:** v4.0

### Motivação
Com múltiplos processadores (APs) executando em paralelo, threads de kernel distintas podiam acessar concorrentemente os mesmos registradores físicos da controladora IDE (`0x1F0`–`0x1F7`) durante leituras e escritas de blocos de disco. Isso resultava em corrupção catastrófica de comandos físicos e dados de disco.

### Alternativas Consideradas
- **Fila de Requisições Assíncrona:** Fila no kernel com thread dedicada para processamento. Arquitetura limpa, mas complexa e de alto overhead.
- **Exclusão Mútua de Baixo Nível (Adotada):** Um mutex global no driver protege os registradores e a sequência de comandos IDE.

### Solução Adotada
Implementação do mutex `ata_mutex` em `src/drivers/ata.c`. Qualquer operação de leitura/escrita física no disco ATA obtém o mutex antes de manipular as portas de E/S da controladora e o libera somente após a conclusão da transferência física de dados (via `insw`/`outsw`) e sincronização de cache.

### Impacto
- Proteção absoluta contra race conditions no nível de hardware do disco.
- Serialização das requisições de E/S de disco. Em sistemas com carga intensa de disco e múltiplos cores, o acesso ao disco torna-se um gargalo de execução.

### Arquivos Envolvidos
- `src/drivers/ata.c`

---

## 4. Buffering de Saída Padrão no Printf de Usuário
* **Data:** 2026-07-05
* **Versão:** v4.1-dev

### Motivação
A implementação original de `printf` da ulibc emitia cada caractere individualmente utilizando a chamada de sistema `SYS_WRITE` (com fd = 1). Isso causava milhares de trocas de privilégio (Ring 3 -> Ring 0 -> Ring 3) para mensagens simples, degradando severamente a performance do console e das aplicações.

### Alternativas Consideradas
- **Write-through direto (Legado):** Simples, mas extremamente ineficiente.
- **Buffering em espaço de usuário (Adotada):** Agrupar caracteres em um buffer e emitir tudo em uma única syscall de escrita.

### Solução Adotada
Introdução da estrutura `struct printf_buffer` alocada na pilha do processo de usuário contendo um buffer de 2048 bytes. O `printf` armazena os caracteres formatados no buffer e invoca a chamada de sistema `SYS_WRITE` somente quando o buffer atinge sua capacidade máxima ou quando a string de formatação é totalmente processada.

### Impacto
- Ganho de performance massivo em operações de escrita no console ou arquivos.
- A alocação local na pilha do buffer é thread-safe e evita alocações no heap em Ring 3.
- Alteração sutil de comportamento: a saída não é descarregada caractere por caractere (comportamento padrão de buffered E/S).

### Arquivos Envolvidos
- `src/user/ulibc.c`
- `include/stdio.h`
