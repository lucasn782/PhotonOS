# Interrupções e Tratamento de Exceções

Este documento especifica o layout da Tabela de Descritores de Interrupção (IDT), os stubs de exceção, os requisitos de EOI e a pilha dedicada IST para Double Fault.

---

## 1. Layout dos Gates da IDT

O PhotonOS inicializa a Tabela de Descritores de Interrupção (`IDT`) com 256 entradas em `idt_init()`:

```c
static void idt_init(void)
{
    memory_set(idt, 0, sizeof(idt));
    idt_set_gate(IRQ_TIMER_VECTOR, timer_irq_stub);       // 0x20 (PIT)
    idt_set_gate(IRQ_KEYBOARD_VECTOR, keyboard_irq_stub); // 0x21 (PS/2 Keyboard)
    idt_set_gate(0x2C, mouse_irq_stub);                   // 0x2C (PS/2 Mouse)
    idt_set_gate(8, double_fault_stub);                   // 0x08 (Double Fault)
    idt[8].ist = 1;                                       // Atribui IST 1
    idt_set_gate(13, gpf_stub);                           // 0x0D (General Protection Fault)
    idt_set_gate(14, page_fault_stub);                    // 0x0E (Page Fault)
    idt_set_gate(0x79, tlb_shootdown_stub);               // 0x79 (TLB Shootdown IPI)
    idt_set_gate(0xFF, spurious_irq_stub);                // 0xFF (LAPIC Spurious)
    idt_load();
}
```

---

## 2. Stubs de Rotinas de Serviço de Interrupção (ISR)

Todas as interrupções de hardware e exceções são declaradas como stubs em Assembly em `src/boot/kernel.asm`.

Esses stubs:
1. Empilham os registradores voláteis para preservar o contexto.
2. Alinham a pilha a 16 bytes para conformidade com a ABI AMD64 System V.
3. Chamam o handler C correspondente em Ring 0.
4. Restauram os registradores.
5. Enviam um sinal de End-Of-Interrupt (`EOI`) ao PIC e LAPIC (via `pic_send_eoi`).
6. Executam `iretq` para retornar da interrupção.

Para exceções que empilham um código de erro (como General Protection Fault ou Page Fault), o stub descarta o código de erro da pilha antes de executar `iretq`.

---

## 3. Double Fault e Configuração de Pilha IST

Um double fault (`#DF`, Vetor 8) ocorre quando a CPU falha ao invocar um handler de exceção anterior (por exemplo, devido a um estouro de pilha do kernel).

Para tratar double faults com segurança sem causar um crash no processador (triple fault):
1. Uma pilha dedicada do **Interrupt Stack Table (IST 1)** é configurada em `tss.c` e atribuída ao gate 8 da IDT.
2. Quando um double fault ocorre, a CPU automaticamente alterna para o ponteiro de pilha físico designado no IST 1 da TSS, contornando a pilha do kernel que estourou.
3. O `double_fault_handler()` imprime registradores de diagnóstico (RIP, CS, RSP, SS, CR2, CR3) e interrompe a execução (`cli; hlt`).

---

## 4. Vetor de Interrupção Espúria

A interrupção espúria do LAPIC é mapeada no Vetor `0xFF`. O `spurious_irq_stub` roteia para `spurious_handler()`, que retorna imediatamente sem escrever no registrador EOI do LAPIC, em conformidade com as especificações x86_64.

---

## 5. Isolamento de Falhas Ring 3

Para evitar que programas de usuário com bugs provoquem panic no kernel:
- **GPF (`gpf_handler`)** e **Page Fault (`vmm_page_fault_handler`)** verificam o segmento de código da falha (`CS`):
  ```c
  if ((cs & 0x3ULL) == 0x3ULL) {
      klog("kernel: exception from userspace, terminating task\n");
      scheduler_exit_current(-1);
      for (;;) {
          __asm__ volatile ("sti; hlt");
      }
  }
  ```
- Se a exceção ocorreu no espaço do usuário (nível de privilégio 3), o kernel encerra a tarefa ofensora em vez de executar um kernel panic.
