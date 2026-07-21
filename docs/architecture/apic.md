# Local APIC (LAPIC)

Este documento descreve o design, os registradores, a configuração e a operação do Local APIC (LAPIC) no PhotonOS.

---

## 1. Visão Geral do Local APIC

O Local APIC (LAPIC) reside em cada núcleo da CPU e gerencia:
- Interrupções de hardware e software locais.
- Interrupções Interprocessador (IPIs) para coordenação SMP (boot de APs, TLB Shootdowns).
- Interrupções de temporizador local.
- Desativação do controlador PIC 8259 legado.

---

## 2. Mapeamento de Memória e Registradores do LAPIC

O endereço base físico do LAPIC é lido do MSR `IA32_APIC_BASE` (`0x1B`):

```c
uint64_t apic_base_msr = rdmsr(0x1B);
lapic_base_addr = apic_base_msr & 0xFFFFF000ULL;
```

Este endereço (padrão: `0xFEE00000`) é mapeado pelo VMM com cache desabilitado (`PAGE_CACHE_DISABLE`) para garantir leituras e escritas diretas (MMIO). Os registradores principais incluem:

| Offset | Nome | Descrição |
| :---: | :--- | :--- |
| `0x20` | APIC ID Register | Identificador APIC do núcleo local |
| `0x30` | APIC Version Register | Versão e detalhes do LAPIC |
| `0xB0` | EOI Register | Escrita de sinal de End-Of-Interrupt |
| `0xF0` | Spurious Interrupt Vector | Vetor de interrupção espúria e bit de habilitação APIC |
| `0x300` | ICR Low (bits 0-31) | Parte inferior do comando de transmissão IPI |
| `0x310` | ICR High (bits 32-63) | Parte superior do alvo de destino IPI |
| `0x350` | LVT LINT0 Register | Tabela de vetores locais para entrada LINT0 |
| `0x360` | LVT LINT1 Register | Tabela de vetores locais para entrada LINT1 |

---

## 3. Inicialização e Configuração ExtINT / NMI

Durante `apic_init()` (BSP) e `apic_init_ap()` (APs secundários):
1. **Desativação do PIC:** Escrita de `0xFF` nas portas `0x21` (PIC Master) e `0xA1` (PIC Slave) para mascarar todas as interrupções legadas.
2. **Habilitação do APIC:** Seta o bit 8 (APIC Software Enable) no registrador de vetor espúrio (`0xF0`) e mapeia o vetor `0xFF`.
3. **Configuração de LVT LINT0 e LINT1:**
   - `LINT0` é configurado em modo `ExtINT` (modo de entrega `111b`, valor `0x700`) para rotear interrupções externas legadas através do LAPIC.
   - `LINT1` é configurado em modo `NMI` (modo de entrega `100b`, valor `0x400`) para rotear interrupções não mascaráveis.

---

## 4. Interrupções Interprocessador (IPIs)

As IPIs são enviadas escrevendo no ICR (Interrupt Command Register):
- **Destino Individual:** Escreve-se o APIC ID alvo no `ICR_HIGH` (bits 56-63). Escreve-se o modo de entrega, nível de asserção e vetor no `ICR_LOW`.
- **Todos Exceto o Remetente (Shorthand):** Escreve-se o destination shorthand `0b11` nos bits 18-19 do `ICR_LOW`.
  - Utilizado para **TLB Shootdowns** (vetor `0x79`) para invalidar caches de tradução em todos os outros núcleos ativos:
  ```c
  apic_write(0x300, 0x000C0000 | 0x79);
  ```
- **Tratador de Vetor Espúrio:** Interrupções espúrias no vetor `0xFF` são roteadas para `spurious_handler()` em `kernel.c`. Nenhum EOI é enviado ao LAPIC para interrupções espúrias, conforme especificação x86_64.

---

## 5. Arquivos de Implementação

| Arquivo | Descrição |
| :--- | :--- |
| `src/kernel/apic.c` | Inicialização, leitura/escrita de registradores, EOI |
| `include/apic.h` | Definições de offsets de registradores e protótipos |
