# 🧠 Memory Protection & Hardware Execution Safety in PhotonOS

## Visão Geral

Esta especificação técnica detalha os mecanismos de proteção de memória baseados em hardware implementados na arquitetura **x86_64 Long Mode** do **PhotonOS**, abrangendo **CR0.WP (Write Protect)**, **EFER.NXE / Bit NX (No-Execute)**, a política **W^X (Write XOR Execute)**, barreiras de memória e coerência do TLB.

---

## ✍️ 1. Proteção de Escrita no Kernel (CR0.WP)

### Descrição Hardware
O bit 16 do registrador de controle `CR0` é o bit **WP (Write Protect)**.

- Quando `CR0.WP = 0`: O código executando em Ring 0 (CPL 0) pode gravar em páginas marcadas como Somente-Leitura (`PAGE_WRITABLE = 0`).
- Quando `CR0.WP = 1`: O processador impede rigorosamente que o código em Ring 0 grave em qualquer página de memória cuja entrada na tabela de páginas (`PTE`) não possua o bit `PAGE_WRITABLE`. Qualquer tentativa de gravação gera uma exceção de Falha de Página (`#PF`, INT 0x0E) com o bit 1 (W/R) do Error Code ativado.

### Ativação no Boot
O bit `CR0.WP` é ativado precocemente no código assembly de transição de boot ([kernel.asm](file:///c:/Users/lucas/OneDrive/Documentos/PhotonOS/src/boot/kernel.asm)) e reconfirmado na inicialização do subsistema VMM ([vmm.c](file:///c:/Users/lucas/OneDrive/Documentos/PhotonOS/src/kernel/vmm.c)):

```c
void vmm_enable_cr0_wp(void) {
    uint64_t cr0;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= (1ULL << 16);
    __asm__ volatile ("mov %0, %%cr0" : : "r"(cr0) : "memory");
}
```

---

## 🚫 2. Bit NX (No-Execute / Execute Disable) e EFER.NXE

### Descrição MSR e PTE
O **Bit NX (Bit 63 das entradas PTE)** permite marcar páginas de memória física como **Não-Executáveis**. 

- Para que o hardware reconheça o Bit 63 como NX nas tabelas de páginas de 64 bits, o bit 11 (**NXE - No-Execute Enable**) do registrador de modelo específico MSR `IA32_EFER` (`0xC0000080`) deve estar ativado.
- Se o processador tentar buscar e executar instruções de uma página com o bit 63 setado, ocorre uma exceção `#PF` com o bit 4 (I/D - Instruction Fetch) do Error Code setado.

### Ativação
```c
void vmm_enable_efer_nxe(void) {
    uint32_t low, high;
    __asm__ volatile ("rdmsr" : "=a"(low), "=d"(high) : "c"(0xC0000080ULL));
    low |= (1U << 11);
    __asm__ volatile ("wrmsr" : : "a"(low), "d"(high), "c"(0xC0000080ULL) : "memory");
}
```

---

## ⚔️ 3. Política W^X (Write XOR Execute)

O PhotonOS aplica rigorosamente a regra **W^X**: uma página de memória pode ser **Gravável** (`W`) ou **Executável** (`X`), mas **NUNCA AMBAS SIMULTANEAMENTE**.

| Região de Memória | Writable (Bit 1) | NX / No-Execute (Bit 63) | Permissão Efetiva |
| :--- | :---: | :---: | :---: |
| Código Kernel (`.text`) | `0` | `0` | **Read / Execute (RX)** |
| Dados Kernel (`.data`/`.bss`) | `1` | `1` | **Read / Write (RW + NX)** |
| Stack Kernel & Guard Pages | `1` (ou `0` no Guard) | `1` | **Read / Write + NX** |
| Kernel Heap | `1` | `1` | **Read / Write + NX** |
| Código de Usuário ELF | `0` | `0` | **User Read / Execute (URX)** |
| Stack & Heap de Usuário | `1` | `1` | **User Read / Write + NX** |

---

## 🔄 4. TLB Flush, `invlpg` e Barreiras de Memória

### Invalidação Local (`invlpg`)
Sempre que mapeamentos de páginas ou permissões de PTE são alterados, a entrada no Translation Lookaside Buffer (TLB) da CPU local é invalidada usando a instrução `invlpg`:

```c
static inline void vmm_flush_tlb(uintptr_t addr) {
    __asm__ volatile ("invlpg (%0)" : : "r"(addr) : "memory");
}
```

### Reload do Registrador CR3
A troca de espaço de endereçamento (`vmm_switch_address_space`) carrega um novo valor em `CR3`, promovendo a invalidação global do TLB para entradas não-globais.

### TLB Shootdown em SMP
Em ambientes multi-core, modificações em tabelas de páginas de processos compartilhados geram IPIs de TLB Shootdown (Vetor `0x79`) via Local APIC. Todos os núcleos AP recebem a interrupção, executam `invlpg` na página alvo e confirmam a recepção usando barreiras de sincronização atômicas (`__sync_fetch_and_add`) e compilação (`"memory"` clobber).
