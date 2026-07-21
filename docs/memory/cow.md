# Otimização de Memória via Copy-On-Write (COW) — PhotonOS v3.1

Este documento descreve com rigor técnico de OSdev o design, a implementação e o fluxo operacional do mecanismo de **Copy-On-Write (COW)** introduzido no PhotonOS v3.1 para a chamada de sistema `sys_fork` (Syscall 23). O COW é um pilar fundamental da gestão de memória virtual em sistemas POSIX modernos, eliminando cópias físicas desnecessárias de frames de memória no momento da bifurcação de processos.

---

## 1. Motivação e Contexto Arquitetural

### 1.1 O Problema: Deep-Copy Ingênua no Fork

No modelo anterior (v2.0 / v3.0), a rotina `vmm_clone_address_space` realizava uma **cópia profunda** (*deep-copy*) da totalidade do espaço de endereçamento do usuário durante o `sys_fork`. Para cada página virtual ativa com o bit `VMM_USER` setado no processo pai, o Kernel:

1. Invocava `pmm_alloc()` para alocar um novo frame físico de 4 KiB.
2. Copiava os 4.096 bytes do frame físico do pai para o frame recém-alocado via iteração byte a byte.
3. Mapeava o novo frame na PML4 do processo filho com as mesmas flags.

Este modelo desperdiçava largura de banda de memória e ciclos de processador em processos que imediatamente executam `sys_execve` após o `fork` (padrão fork-exec do POSIX), nunca chegando a modificar as páginas herdadas.

### 1.2 A Solução: Compartilhamento Preguiçoso de Frames

O mecanismo de Copy-On-Write posterga a duplicação física da memória até o exato instante em que um dos processos (pai ou filho) tenta **escrever** em uma página compartilhada. Até que essa escrita ocorra, ambos os processos apontam para o **mesmo frame físico**, drasticamente reduzindo o consumo de memória e o tempo de execução do `fork`.

```
                     ANTES DO COW (v3.0)
  Pai                                        Filho
  PTE[virt] → Frame A (4 KiB)    PTE[virt] → Frame B (cópia de A, 4 KiB)
  [WRITABLE]                                [WRITABLE]

                     DEPOIS DO COW (v3.1)
  Pai                                        Filho
  PTE[virt] → Frame A (4 KiB)    PTE[virt] → Frame A (mesmo frame!)
  [COW, !WRITABLE]                          [COW, !WRITABLE]
                   refcount(A) = 2
```

---

## 2. Pilar A — Contador de Referências de Frames Físicos (PMM)

### 2.1 Estrutura de Dados: `pmm_refcounts`

Para rastrear o compartilhamento de frames entre processos, o Gerenciador de Memória Física (PMM) foi estendido com um array estático de contadores de referência, indexado pelo número de frame físico (PFN — *Physical Frame Number*):

```c
/* memory.c */
#define PMM_MAX_BLOCKS (PMM_TOTAL_MEMORY / PMM_PAGE_SIZE)   /* 128 MiB / 4 KiB = 32.768 frames */

static uint32_t pmm_refcounts[PMM_MAX_BLOCKS];
```

Cada elemento `pmm_refcounts[i]` representa o número de mapeamentos virtuais ativos que apontam para o frame físico `i * PMM_PAGE_SIZE`. O tipo `uint32_t` garante suporte a até 4.294.967.295 referências por frame — suficiente para qualquer cenário realista de bifurcação em cascata.

### 2.2 Ciclo de Vida do Contador

#### Inicialização (`pmm_init`)
Todos os contadores são zerados durante a inicialização do PMM. Os frames pertencentes à região reservada (bootloader, kernel e estruturas críticas do sistema) recebem `refcount = 1`:

```c
for (uint64_t i = 0; i < PMM_MAX_BLOCKS; i++) {
    pmm_refcounts[i] = 0;
}
/* Frames reservados: jamais retornam ao pool livre */
for (uint64_t i = 0; i < pmm_reserved_block_count; i++) {
    bitmap_set(i);
    pmm_refcounts[i] = 1;
}
```

#### Alocação (`pmm_alloc`)
Ao alocar um frame livre, o contador é inicializado em `1`, representando o único mapeamento existente:

```c
bitmap_set(i);
pmm_refcounts[i] = 1;
return (void *)(i * PMM_PAGE_SIZE);
```

#### Liberação Condicional (`pmm_free`)
A rotina `pmm_free` deixa de ser uma liberação incondicional. O frame físico só retorna ao pool livre (bitmap limpo) quando o seu contador de referências alcança **zero**:

```c
void pmm_free(void *ptr) {
    uint64_t block = address / PMM_PAGE_SIZE;
    /* ... validações de bounds ... */
    if (pmm_refcounts[block] > 0) {
        pmm_refcounts[block]--;
        if (pmm_refcounts[block] == 0) {
            bitmap_clear(block);   /* Somente agora o frame é devolvido ao pool */
        }
    }
}
```

### 2.3 Funções Auxiliares de Referência

Duas novas funções públicas compõem a interface do PMM para uso pelo VMM:

```c
/* Incrementa o contador de referências do frame que contém 'ptr' */
void pmm_ref_inc(void *ptr);

/* Retorna o contador de referências atual do frame que contém 'ptr' */
uint32_t pmm_ref_get(void *ptr);
```

Essas funções são declaradas em `include/memory.h` e implementadas em `src/kernel/memory.c`.

---

## 3. Pilar B — Clonagem Preguiçosa do Espaço de Endereçamento (VMM)

### 3.1 Refatoração de `vmm_clone_address_space`

A função `vmm_clone_address_space` em `src/kernel/vmm.c` foi completamente refatorada. A varredura dos quatro níveis hierárquicos de paginação x86_64 (PML4 → PDPT → PD → PT) permanece idêntica para garantir a integridade da travessia, mas a semântica das operações sobre cada PTE válida foi radicalmente alterada.

#### Regiões Preservadas (Sem Alteração)

- **Entrada 0 da PML4 (Identity Map de Boot)**: Copiada por referência direta — cobre estruturas de boot precoce e o MMIO do driver e1000 em `0xF0000000`.
- **Entradas 256–511 da PML4 (Higher-Half do Kernel)**: Compartilhadas diretamente — cobrem o heap do kernel, o framebuffer gráfico, o LAPIC e todos os mapeamentos Ring 0.

#### Região COW (Entradas 1–255 da PML4 — Espaço de Usuário)

Para cada PTE válida com `VMM_USER` setado encontrada na varredura profunda, o kernel executa o protocolo de compartilhamento COW:

```c
uint64_t parent_entry = parent_pt[pt_i];
uint64_t phys_addr    = parent_entry & VMM_ENTRY_ADDR_MASK;
uint64_t flags        = parent_entry & VMM_ENTRY_FLAGS_MASK;

/* Páginas graváveis: aplica degradação de privilégio COW */
if (flags & PAGE_WRITABLE) {
    flags &= ~PAGE_WRITABLE;   /* Remove permissão de escrita */
    flags |=  PAGE_COW;        /* Ativa bit de controle COW (bit 9 = 0x200) */
    parent_pt[pt_i] = phys_addr | flags;   /* Modifica a PTE do PAI */
}

/* Mapeia o mesmo frame físico no espaço do FILHO com as flags ajustadas */
map_in_pml4(child_pml4, virt, phys_addr, (uint32_t)flags);

/* Invalida a entrada TLB do BSP para o endereço virtual modificado */
vmm_flush_tlb(virt);

/* Registra o compartilhamento no contador de referências do PMM */
pmm_ref_inc((void *)phys_addr);
```

### 3.2 Manipulação das Flags da PTE

O mecanismo COW opera exclusivamente no nível das flags de 12 bits da PTE (bits 0–11), conforme a especificação do formato de PTE do x86_64:

| Bit | Máscara   | Nome            | Semântica COW                                                 |
|-----|-----------|-----------------|---------------------------------------------------------------|
| 0   | `0x001`   | `PAGE_PRESENT`  | Mantido — página continua acessível para leitura              |
| 1   | `0x002`   | `PAGE_WRITABLE` | **Removido** — provoca `#PF` na tentativa de escrita          |
| 2   | `0x004`   | `PAGE_USER`     | Mantido — acesso permitido no Ring 3                          |
| 9   | `0x200`   | `PAGE_COW`      | **Setado** — sinaliza ao handler de `#PF` que é uma falha COW |

> **Nota arquitetural**: O bit 9 pertence ao intervalo de bits "disponíveis para uso do sistema operacional" (`OS-Available`, bits 9–11) definido pelo Intel SDM Vol. 3A, §4.5. O PhotonOS utiliza este bit exclusivamente para o mecanismo COW — ele não possui semântica definida pelo hardware e é ignorado pela MMU durante a tradução de endereços normais.

### 3.3 Diagrama de Fluxo do Fork COW

```
sys_fork() invocado pelo processo pai
         │
         ▼
scheduler_fork_current()
         │
         ├──► Cria nova PML4 para o filho (pmm_alloc)
         │
         ├──► Copia entradas de kernel (0 e 256–511) por referência
         │
         └──► Para cada PTE válida no espaço de usuário (1–255):
                    │
                    ├── PTE com PAGE_WRITABLE?
                    │       ├── Sim: Remove PAGE_WRITABLE, seta PAGE_COW
                    │       │       Modifica PTE do pai com as novas flags
                    │       └── Não: Mantém flags inalteradas (ex: read-only)
                    │
                    ├── Mapeia MESMO frame físico no filho com flags ajustadas
                    ├── vmm_flush_tlb(virt) — invalida TLB local (BSP)
                    └── pmm_ref_inc(phys) — incrementa refcount do frame

         │
         ▼
    (se APs ativos)
    Dispara IPI TLB Shootdown via LAPIC (Vector 0x79)
         │
         ▼
    Retorna child_pml4 ao escalonador → filho entra na fila TASK_READY
```

---

## 4. Pilar C — Tratamento da Exceção INT 0x0E (Page Fault / #PF)

### 4.1 Registro na IDT

O vetor de exceção 14 (`INT 0x0E`, `#PF`) foi adicionado à tabela IDT durante a inicialização do sistema em `idt_init()`:

```c
/* kernel.c — idt_init() */
idt_set_gate(14, page_fault_stub);
```

### 4.2 Stub Assembly `page_fault_stub` (`kernel.asm`)

Quando o processador detecta uma violação de proteção de página, o hardware realiza automaticamente as seguintes ações antes de transferir o controle para o handler registrado na IDT:

1. **Salva o estado da CPU** (RIP, CS, RFLAGS, RSP, SS) no topo da pilha de kernel atual.
2. **Empurra o Código de Erro** (`error_code`) — um inteiro de 32 bits codificando o tipo de falha.
3. **Carrega em CR2** o endereço virtual linear que causou a falha.

O stub assembly preserva todos os registradores de propósito geral e chama a rotina em C:

```nasm
page_fault_stub:
    ; Na entrada: [rsp+0] = error_code, [rsp+8] = RIP, ...
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8 .. push r15          ; 15 registradores salvos = 120 bytes

    mov rdi, [rsp + 120]         ; arg1: error_code (sysV AMD64 ABI)
    mov rsi, cr2                 ; arg2: fault_addr (endereço em CR2)
    mov rdx, [rsp + 128]         ; arg3: RIP faultante

    mov rbp, rsp
    sub rsp, 8
    and rsp, -16                 ; Alinhamento de pilha (ABI AMD64)
    cld
    call vmm_page_fault_handler
    mov rsp, rbp

    pop r15 .. pop rax           ; Restaura registradores
    add rsp, 8                   ; Descarta error_code
    iretq                        ; Retorna ao espaço de usuário
```

### 4.3 Rotina C `vmm_page_fault_handler` (`vmm.c`)

A rotina C implementa a lógica de resolução da falha de proteção de página:

#### Etapa 1 — Validação dos Critérios COW

A exceção só é tratada como uma falha COW legítima se **ambas** as condições forem satisfeitas:

| Critério | Verificação | Descrição |
|----------|-------------|-----------|
| Falha por escrita | `error_code & 0x02` | Bit 1 do código de erro setado |
| Página COW | `pte & PAGE_COW` | Bit 9 da PTE setado |

```c
/* Verifica se é uma escrita (bit 1 do código de erro) com bit PAGE_COW setado */
if ((error_code & 0x02) && (pte & PAGE_COW)) {
    is_cow = 1;
}
```

Se qualquer um dos critérios falhar, a exceção é tratada como um acesso inválido irrecuperável e o kernel entra em pânico.

#### Etapa 2 — Divisão de Frame (*Page Splitting*)

O comportamento diverge com base no contador de referências do frame físico envolvido:

##### Caso A: `refcount > 1` — Frame Compartilhado (Separação Necessária)

```c
void *new_frame = pmm_alloc();      /* Aloca novo frame no PMM */

/* Cópia atômica de 4 KiB: frame antigo → novo frame */
uint8_t *src = (uint8_t *)old_phys_frame;
uint8_t *dst = (uint8_t *)new_frame;
for (int i = 0; i < 4096; i++) {
    dst[i] = src[i];
}

/* Atualiza PTE do processo atual: aponta para o novo frame */
uint64_t flags = pte & VMM_ENTRY_FLAGS_MASK;
flags &= ~PAGE_COW;       /* Remove bit COW */
flags |=  PAGE_WRITABLE;  /* Restaura permissão de escrita */
pt[pt_index] = ((uint64_t)new_frame & VMM_ENTRY_ADDR_MASK) | flags;

/* Decrementa o refcount do frame antigo */
pmm_free((void *)old_phys_frame);
```

##### Caso B: `refcount == 1` — Frame Exclusivo (Reaproveitamento In-Place)

Quando o contador indica que apenas um processo aponta para o frame, não há necessidade de alocação: o kernel simplesmente restaura os privilégios na PTE existente:

```c
/* Caso otimizado: único detentor do frame — nenhuma alocação extra */
uint64_t flags = pte & VMM_ENTRY_FLAGS_MASK;
flags &= ~PAGE_COW;       /* Remove bit COW */
flags |=  PAGE_WRITABLE;  /* Restaura permissão de escrita */
pt[pt_index] = old_phys_frame | flags;
/* pmm_free NÃO é chamado — refcount permanece em 1 */
```

#### Etapa 3 — Invalidação do TLB Local

Após a atualização da PTE, o TLB (Translation Lookaside Buffer) pode ainda conter uma entrada cacheada com as flags antigas (`!WRITABLE`). A instrução `invlpg` invalida seletivamente apenas a entrada correspondente ao endereço falho:

```c
vmm_flush_tlb(fault_addr);   /* → invlpg (%rdi) */
```

#### Diagrama de Decisão do Handler

```
  CPU detecta violação de proteção → empurra error_code e fault_addr em CR2
              │
              ▼
      page_fault_stub (ASM)
              │ salva registradores, extrai error_code e CR2
              ▼
      vmm_page_fault_handler(error_code, fault_addr, rip, cs)
              │
              ├── error_code & 0x02? (escrita?)   ─── NÃO ──┐
              │        │ SIM                                │
              ├── pte & PAGE_COW? ─────────────── NÃO ───┼──► Unresolved Fault:
              │        │ SIM                                │    Origem Ring 3 (cs & 3 == 3)?
              │                                             │      ├─ SIM: scheduler_exit_current(-1)
              ├── pmm_ref_get(old_frame)                    │      └─ NÃO: KERNEL PANIC
              │        │                                    │
              │   refcount > 1?                             │
              │    ├─ SIM: pmm_alloc → cópia 4 KiB ─────────┤
              │    │       → atualiza PTE → pmm_free(old)   │
              │    └─ NÃO: atualiza PTE in-place            │
              │                                             │
              └── vmm_flush_tlb(fault_addr) ────────────────┘
                  → iretq (retoma execução do usuário)
```

#### Isolamento de Falhas do Ring 3 (v4.1)

Para prevenir que programas de usuário malcomportados ou com bugs de estouro de pilha e acesso ilegal de memória provoquem pânico geral no kernel (Kernel Panic), o PhotonOS v4.1 implementa isolamento de privilégio na rotina `vmm_page_fault_handler`.

Caso a falha de página não possa ser resolvida via Copy-On-Write, o kernel avalia o registrador `CS` (Code Segment) do frame de interrupção:
- **Se a exceção originou-se em Ring 3** (`(cs & 3) == 3`), o processo de usuário é finalizado imediatamente chamando `scheduler_exit_current(-1)` de forma limpa.
- **Se ocorreu em Ring 0**, a falha representa um erro crítico interno do kernel e prossegue para a rotina de KERNEL PANIC.

---

## 5. Pilar D — Consistência Multicore: TLB Shootdown via APIC

### 5.1 O Problema de Coerência em Ambientes SMP

Em sistemas multiprocessados, cada núcleo mantém seu próprio **TLB** — um cache de hardware que armazena traduções recentes de endereços virtuais para físicos. Após a modificação das PTEs do processo pai durante `vmm_clone_address_space`, os núcleos AP (*Application Processors*) em execução podem ainda manter entradas TLB obsoletas (`WRITABLE`, sem `COW`) para as páginas modificadas.

Se um AP executasse uma instrução de escrita em uma dessas páginas com uma entrada TLB obsoleta, a CPU não geraria o `#PF` esperado — violando a semântica do COW silenciosamente.

### 5.2 Implementação: IPI de Invalidação de TLB

Após a conclusão da travessia de tabelas de páginas em `vmm_clone_address_space`, o código verifica se há APs ativos e, em caso positivo, dispara um **IPI broadcast** (*Inter-Processor Interrupt*) via LAPIC para forçar a invalidação do TLB em todos os núcleos:

```c
/* vmm.c — vmm_clone_address_space() */
if (smp_ap_booted_count() > 0) {
    apic_write(APIC_REG_ICR_HIGH, 0);
    /* Modo: All Excluding Self (bits 18-19 = 0b11), Nível: Assert,
       Tipo de entrega: Fixed, Vector: 0x79 */
    apic_write(APIC_REG_ICR_LOW, 0x000C4000 | 0x79);
}
```

### 5.3 Handler de Shootdown (`smp_tlb_shootdown_handler`)

Registrado no vetor IDT `0x79`, o handler executa em cada AP receptor:

```c
/* smp.c */
void smp_tlb_shootdown_handler(void)
{
    uint64_t cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile ("mov %0, %%cr3" : : "r"(cr3) : "memory");
    /* Recarregar CR3 com o mesmo valor invalida todo o TLB do núcleo */
    apic_eoi();   /* Sinaliza EOI ao LAPIC local para limpar a linha de IRQ */
}
```

O recarregamento de `CR3` com o mesmo valor é uma técnica documentada pelo Intel SDM (Vol. 3A, §4.10.4.1) para invalidação completa e atômica do TLB sem necessidade de `INVLPG` individual.

### 5.4 Diagrama de Sequência: TLB Shootdown

```
BSP (vmm_clone_address_space)               AP1                    AP2
         │                                   │                      │
         │ Modifica PTEs (remove WRITABLE,   │                      │
         │ seta COW) para todas as páginas   │                      │
         │ do espaço de usuário              │                      │
         │                                   │                      │
         │── apic_write(ICR_HIGH, 0) ──────────────────────────────►│
         │── apic_write(ICR_LOW, 0x000C4079) ─────────────────────►│
         │                          IPI Vector 0x79                  │
         │                                   │ tlb_shootdown_stub    │
         │                                   │ reload CR3 → TLB flush│
         │                                   │ apic_eoi()            │
         │                                             tlb_shootdown_stub
         │                                             reload CR3 → TLB flush
         │                                             apic_eoi()
         │
         └── Retorna child_pml4 ao escalonador
```

---

## 6. Integração com `vmm_destroy_address_space`

A destruição do espaço de endereçamento de um processo (chamada em `sys_exit` / `scheduler_terminate_task`) também foi implicitamente beneficiada. Ao percorrer as tabelas de páginas para liberar frames via `pmm_free`, a semântica de decremento do refcount garante que frames ainda compartilhados com outros processos (por exemplo, se o filho ainda estiver ativo) **não sejam prematuramente devolvidos ao pool livre**.

Este comportamento emergente do COW previne a classe de vulnerabilidades conhecida como **Use-After-Free** em páginas compartilhadas entre processos relacionados.

---

## 7. Impacto de Performance e Análise de Complexidade

| Operação | v3.0 (Deep-Copy) | v3.1 (COW) |
|----------|------------------|------------|
| `sys_fork` — custo de tempo | `O(n)` páginas alocadas + copiadas | `O(n)` pages percorridas, `0` alocações |
| `sys_fork` — custo de memória | `n × 4 KiB` novos frames | `0` novos frames (apenas entradas compartilhadas) |
| Primeira escrita pós-fork | Instantânea (página já exclusiva) | `O(1)` — uma alocação + cópia de 4 KiB + `invlpg` |
| `sys_exec` após `fork` (fork-exec) | `n × 4 KiB` desperdiçados | `0 KiB` — nenhuma cópia antes do `exec` |
| Consumo de memória (200 processos fork) | `200 × n × 4 KiB` | `n × 4 KiB` (frames compartilhados) |

---

## 8. Restrições de Design e Invariantes Ring 0

1. **Proibição de formatação em `klog`**: Nenhuma mensagem emitida pelo handler de `#PF` ou pelas rotinas de fork utiliza especificadores de formato (`%d`, `%x`, `%s`). Toda saída de diagnóstico é composta por strings literais estáticas, em conformidade com o contrato Ring 0 do PhotonOS.

2. **Imunidade do Espaço de Kernel**: As entradas 0 e 256–511 da PML4 (kernel space e identity map) nunca são submetidas ao protocolo COW. Páginas de kernel jamais têm `PAGE_WRITABLE` removido nem `PAGE_COW` ativado.

3. **Alinhamento de Pilha ABI**: O stub assembly de `#PF` (`page_fault_stub`) realiza o alinhamento de pilha a 16 bytes (`and rsp, -16`) antes da chamada à rotina C, em conformidade com a ABI System V AMD64.

4. **Estabilidade de Assinaturas Públicas**: As assinaturas de `pmm_alloc`, `pmm_free`, `vmm_clone_address_space` e das syscalls públicas permanecem imutáveis, garantindo compatibilidade total com as ferramentas do espaço de usuário em Ring 3.

---

## 9. Referências e Especificações

- **Intel® 64 and IA-32 Architectures Software Developer's Manual**, Vol. 3A — §4.5 (Page-Directory and Page-Table Entries), §4.10 (Caching Translation Information), §10.6 (IPI)
- **OSDev Wiki** — [Copy-On-Write](https://wiki.osdev.org/Copy-On-Write), [TLB Shootdown](https://wiki.osdev.org/TLB_Shootdown)
- Código-fonte: [`src/kernel/vmm.c`](../src/kernel/vmm.c), [`src/kernel/memory.c`](../src/kernel/memory.c), [`src/kernel/smp.c`](../src/kernel/smp.c), [`src/boot/kernel.asm`](../src/boot/kernel.asm)
