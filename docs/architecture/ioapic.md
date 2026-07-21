# I/O APIC e Roteamento de Interrupções

Este documento detalha a estratégia de roteamento de interrupções de hardware no PhotonOS, destacando a abordagem baseada em ExtINT e a ausência de driver para o I/O APIC.

---

## 1. Estratégia de Roteamento de Interrupções

Em sistemas x86_64 modernos, as interrupções de hardware são tipicamente roteadas de periféricos para núcleos de CPU individuais via **I/O APIC** usando tabelas de redirecionamento (Redirection Table Entries - RTEs).

No entanto, o PhotonOS v4.1 **não** implementa um driver para o I/O APIC. Em vez disso, utiliza um **mecanismo de roteamento legado baseado em ExtINT** através da entrada LINT0 do Local APIC:

```text
┌──────────────┐
│  Periféricos │ (Teclado, Mouse, PIT Timer)
└──────┬───────┘
       │ IRQs 0, 1, 12
       ▼
┌──────────────┐
│  8259 PIC    │ (Desmascarado para IRQs selecionados)
└──────┬───────┘
       │ Sinal ExtINT
       ▼
┌──────────────┐
│  LINT0 PIN   │ (Entrada do LAPIC configurada como ExtINT)
└──────┬───────┘
       │ Vetores 0x20, 0x21, 0x2C
       ▼
┌──────────────┐
│  Núcleo CPU  │ (Despacho de interrupção)
└──────────────┘
```

---

## 2. Re-habilitação Seletiva do PIC Legado

Embora `apic_init()` inicialmente mascare todos os registradores do PIC, o kernel subsequentemente reconfigura-os para os dispositivos necessários em `pic_init_irqs()`:

- **Registrador de Máscara do PIC Master (`0x21`)**: Configurado para `0xF8` (habilita IRQ 0 - Timer, IRQ 1 - Teclado, IRQ 2 - Cascata).
- **Registrador de Máscara do PIC Slave (`0xA1`)**: Configurado para `0xEF` (habilita IRQ 12 - Mouse PS/2).

---

## 3. Configuração do LINT0 no LAPIC

Ao configurar `LINT0` em modo ExtINT no Local APIC, o processador permite que o PIC legado contorne o roteamento do I/O APIC e dispare vetores diretamente:

```c
/* apic.c */
apic_write(APIC_REG_LVT_LINT0, 0x700); // 0x700 = modo de entrega ExtINT
```

Isso garante que as seguintes interrupções alcancem os núcleos do processador:
- **PIT Timer**: Dispara o Vetor `0x20` (IRQ 0).
- **Teclado**: Dispara o Vetor `0x21` (IRQ 1).
- **Mouse**: Dispara o Vetor `0x2C` (IRQ 12).

---

## 4. Limitações Atuais

- **Sem Roteamento Multi-core:** Todas as interrupções de hardware roteadas através de ExtINT devem ser tratadas pelo Bootstrap Processor (BSP), pois as linhas do PIC legado só mapeiam para a entrada LINT0 da CPU primária. Os núcleos secundários (APs) não recebem interrupções de hardware físicas diretamente.
- **Roadmap Futuro:** A distribuição verdadeira de interrupções de hardware multicore requer o mapeamento dos registradores do I/O APIC (tipicamente no endereço `0xFEC00000`) e a configuração de entradas de tabela de redirecionamento (RTEs) para as linhas de teclado, mouse e e1000 PCI.
