# 🛡️ Kernel Security Architecture in PhotonOS

## Visão Geral

O **PhotonOS** adota um modelo defense-in-depth para garantir a integridade do kernel antes do processamento de entradas não confiáveis do espaço de usuário (Ring 3). Esta especificação descreve os mecanismos de segurança implementados no kernel, incluindo proteção contra estouro de pilha (Stack Canaries), validação de ponteiros de chamadas de sistema, sanitização de memória no Heap (Poisoning e detecção de Double Free) e prevenção de corrupção da pilha do kernel via Guard Pages.

---

## 🔒 1. Stack Canary (-fstack-protector-strong)

### Mecanismo de Funcionamento
O kernel é compilado com a flag `-fstack-protector-strong` do GCC. O compilador insere automaticamente um valor de salvaguarda (canário de pilha) no prólogo das funções vulneráveis entre os buffers locais e os ponteiros de retorno/frame base (`RBP`/`RIP`). Os binários Ring 3 freestanding são compilados com `-fno-stack-protector` enquanto o runtime não configurar TLS/FS-base por tarefa; sem essa base, o prólogo padrão do GCC acessaria `FS:0x28` e causaria `#PF` no usuário.

- **Símbolo do Guardião no Kernel**: `uintptr_t __stack_chk_guard = 0x595A5B5C5D5E5F60ULL;`
- **Manipulador de Falha no Kernel**: `void __stack_chk_fail(void)`
  - Desativa interrupções (`cli`).
  - Emite log de **KERNEL PANIC: STACK SMASHING DETECTED**.
  - Entra em loop de travamento seguro (`hlt`).
- **Ring 3 (`ulibc`)**: canário desabilitado temporariamente por ausência de TLS/FS-base. A reativação depende de contexto FS por processo.

---

## ☣️ 2. Validação e Sanitização do Heap Kernel

O alocador de memória do kernel ([heap.c](file:///c:/Users/lucas/OneDrive/Documentos/PhotonOS/src/kernel/heap.c)) implementa validações em runtime e sanitização preventiva para mitigar vulnerabilidades de gerenciamento de memória dinâmica.

### Detecção de Double Free
Ao chamar `kfree(ptr)`:
1. O cabeçalho `struct heap_block` antecedente é validado.
2. Se o bloco já possuir a flag `free == 1` ou o magic setado como `HEAP_FREED_MAGIC` (`0xDEADBEEF48454150ULL`), o kernel registra um alerta **DOUBLE FREE DETECTED at <ptr>** e interrompe a execução do `kfree` sem corromper a lista duplamente encadeada.

### Detecção de Use-After-Free (UAF Poisoning)
Ao liberar um bloco de memória via `kfree(ptr)`:
- Toda a área de payload útil (`block->size` bytes) é sobrescrita com o byte de veneno `0xDD` (`HEAP_POISON_BYTE`).
- Tentativas subsequentes de leitura de ponteiros ou dados no bloco liberado resultarão em valores ilegíveis ou falhas previsíveis de proteção em vez de reutilização de dados sensíveis.

### Verificação de Integridade (`heap_validate()`)
A função `heap_validate()` percorre a lista de blocos do heap verificando:
- Consistência de números mágicos (`HEAP_MAGIC` vs `HEAP_FREED_MAGIC`).
- Validade dos ponteiros encadeados (`curr->next->prev == curr`).
- Limites operacionais (`heap_start <= block < heap_end`).

---

## 🔍 3. Validação de Mapeamento e Permissões de Ponteiros de Syscall

Para evitar ataques de substituição de ponteiro (confused deputy, dereferenciamento de ponteiros nulos ou modificação maliciosa do espaço do kernel por chamadas de sistema), o despachante central `syscall_handler` valida todos os ponteiros recebidos do Ring 3 antes de repassá-los aos subsistemas internos do kernel.

### Funções de Validação no VMM
- `int vmm_validate_user_ptr(const void *ptr, size_t size, int write_intent)`
  - Garante `ptr != NULL` e sem estouro numérico (`ptr + size >= ptr`).
  - Restringe o endereço ao limite canônico de usuário (`< 0x0000800000000000ULL`).
  - Varre todas as páginas no intervalo `[ptr, ptr + size)` nas tabelas PML4/PDPT/PD/PT.
  - Verifica se cada página possui os bits `PAGE_PRESENT` e `PAGE_USER`.
  - Se `write_intent == 1`, confirma que a página possui `PAGE_WRITABLE` ou `PAGE_COW`.
- `int vmm_validate_user_string(const char *str, size_t max_len)`
  - Garante que a string terminada em `\0` reside inteiramente no espaço de usuário com permissão de leitura.

---

## 🧱 4. Kernel Stack Guard Pages

Cada tarefa criada pelo escalonador possui uma pilha de kernel alocada de 8 KiB.

- **Página 0 (Endereço Inferior)**: Guard Page não-presente (`PAGE_PRESENT = 0`, `PAGE_NX = 1`).
- **Página 1 (Endereço Superior)**: Área utilizável da pilha de kernel (`RSP` inicial no topo da Página 1).
- **Efeito de Estouro**: Se uma execução de kernel estourar a pilha consumindo mais de 4 KiB, o decremento de `RSP` acessará a Guard Page. O hardware gerará imediatamente um Page Fault (`#PF`, INT 0x0E). O manipulador `vmm_page_fault_handler` identifica a falha na Guard Page e encerra o processo de forma isolada sem corromper estruturas adjacentes.
