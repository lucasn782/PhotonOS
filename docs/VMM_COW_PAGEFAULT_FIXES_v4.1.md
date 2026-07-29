# PhotonOS v4.1 — Auditoria e correções de VMM/COW/Page Fault

## Escopo

Esta auditoria revisa a travessia das tabelas de páginas, clonagem de espaços de endereçamento, Copy-On-Write (COW), tratamento de `#PF`, guard pages e o descarte de tarefas. As mudanças são deliberadamente cirúrgicas: não houve alteração de interfaces públicas nem mudança da estratégia de alocação de stacks.

## Causa raiz confirmada

Após `fork()`, pai e filho registram os frames de usuário em `task->user_physical_pages[]` e compartilham cada frame COW com refcount incrementado. Quando uma escrita separava um frame, o handler atualizava somente a PTE para o novo frame e decrementava o refcount do antigo. O inventário da tarefa continuava contendo o endereço antigo.

Na saída posterior dessa tarefa, `release_user_pages()` chamava `pmm_free()` para o endereço antigo pela segunda vez. Isso podia liberar um frame ainda mapeado pelo processo irmão; uma reutilização subsequente do frame produzia corrupção de memória e podia se manifestar como Page Fault não resolvido.

## Correções aplicadas

- `src/kernel/vmm.c`: a divisão COW substitui o frame antigo pelo novo em `user_physical_pages[]` da tarefa corrente antes de decrementar o refcount antigo.
- `src/kernel/vmm.c`: COW agora exige explicitamente `PRESENT=1`, `WRITE=1` no error code e `PAGE_COW=1` na PTE. Falhas não-presentes, de leitura, de execução NX ou por bit reservado não entram no fluxo COW.
- `src/kernel/elf.c`: flags encaminhadas para o VMM usam `uint64_t`; assim, qualquer flag de PTE de alta ordem, incluindo `PAGE_NX`, não sofre truncamento.
- `Makefile`: `USER_CFLAGS` desabilita o stack protector apenas no código Ring 3 freestanding. O prólogo gerado pelo protector lê `FS:0x28`, mas o kernel ainda não inicializa TLS/FS-base para processos; a leitura produzia `#PF` de usuário (`error code 0x5`, endereço `0x28`) antes do shell aceitar comandos. O protector do kernel permanece habilitado.

As PTEs continuam preservando endereço físico e todos os bits aceitos por `VMM_ENTRY_FLAGS_MASK`, incluindo `PAGE_PRESENT`, `PAGE_WRITABLE`, `PAGE_USER`, `PAGE_GLOBAL` (bit 8), `PAGE_COW` (bit 9) e `PAGE_NX` (bit 63).

## Fluxo de Page Fault

```text
CPU: #PF → CR2 contém endereço e CPU empilha error code
page_fault_stub: salva registradores e chama vmm_page_fault_handler
handler: percorre PML4 → PDPT → PD → PT
  PRESENT|WRITE + PAGE_COW?
    sim: aloca/copia se refcount > 1, atualiza PTE e inventário, invlpg
    não: Ring 3 termina somente a tarefa; Ring 0 entra em panic
iretq: retoma a instrução que causou a falha quando COW foi resolvido
```

O stub lê `CR2` diretamente, passa o error code, RIP e CS corretos e executa `iretq` após restaurar os registradores. A IDT usa IST2 para `#PF`; portanto, o diagnóstico não depende da stack potencialmente defeituosa da tarefa.

## Fluxo Copy-On-Write e TLB

`vmm_clone_address_space()` mantém as entradas de kernel e de identidade compartilhadas, cria as tabelas de usuário do filho, remove `PAGE_WRITABLE` de páginas graváveis e acrescenta `PAGE_COW`. A rotina preserva as flags em `uint64_t`, incrementa o refcount e invalida a tradução local por `invlpg`. Com APs inicializados, a rotina também emite IPI de shootdown e aguarda os ACKs; sem AP ativo, esse caminho é ignorado e não há espera por ACK inexistente.

Na primeira escrita, o handler cria um frame exclusivo apenas se o refcount for maior que um. O frame novo passa a constar no inventário da tarefa; o antigo permanece registrado somente nas tarefas que ainda o mapeiam.

## Scheduler e guard pages

As guard pages de kernel são uma estratégia de páginas deliberadamente não-presentes: `scheduler_create_task()`, `scheduler_add_user_process()` e `scheduler_fork_current()` chamam `vmm_unmap()` para a primeira página de cada stack de 8 KiB. Logo, não há mapeamento com `PAGE_NX` isolado a corrigir.

`scheduler_exit_current()` apenas marca a tarefa corrente como zumbi e libera os recursos de usuário após trocar para o CR3 do kernel quando necessário. As stacks vêm do pool estático do scheduler e não são liberadas durante esse caminho. O próximo timer IRQ troca para uma tarefa pronta; nenhum RSP ativo volta a uma stack desalocada.

## Hipóteses auditadas

| Hipótese | Resultado |
|---|---|
| `valid_table_pointer()` rejeita Higher Half | Descartada: a versão auditada aceita ponteiros alinhados não nulos e não impõe o limite `PMM_TOTAL_MEMORY`. Entradas de tabelas continuam validadas como endereços físicos. |
| Clone trunca `PAGE_NX` para `uint32_t` | Descartada em `vmm_clone_address_space()`: as flags já eram `uint64_t`. O encaminhamento de flags ELF foi padronizado para `uint64_t`. |
| Guard page é `PAGE_NX` sem `PAGE_PRESENT` | Descartada: as guard pages são não-presentes por projeto e o scheduler não depende da página removida. |
| Scheduler libera stack em uso | Descartada: o caminho de saída não libera as stacks do pool. |
| Inventário COW fica desatualizado após split | Confirmada e corrigida. |
| Shell falha imediatamente por COW | Descartada: o traço QEMU mostrou leitura de canário em `FS:0x28`, não uma PTE COW. |

## Impacto e riscos conhecidos

A correção restaura a correspondência entre PTE, contador de referências e inventário de liberação da tarefa, eliminando a dupla baixa de referência após um split COW. Persistem os limites de projeto: somente a área de usuário nos índices PML4 1–255 participa de COW, há no máximo `TASK_MAX_USER_PAGES` frames rastreados por tarefa e o shootdown usa um contador global de ACKs.

## Testes realizados

Foram executados `make clean`, `make` e `make fat16-disk` com sucesso. Em QEMU com uma CPU, o boot alcançou VMM, heap, ATA, e1000, APIC, TSS, ELF, scheduler e o prompt do shell; antes da correção de `USER_CFLAGS`, o traço registrou a falha de usuário em `FS:0x28`. Em QEMU com quatro CPUs, a VM reiniciou após o início do bootstrap SMP, antes de TSS/scheduler; esse problema preexistente está fora do caminho COW alterado e impede declarar a validação SMP completa nesta auditoria.
