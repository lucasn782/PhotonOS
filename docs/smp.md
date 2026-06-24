# Inicialização de Multiprocessamento Simétrico (SMP) e Boot de Cores Secundários (v3.0)

Este documento descreve detalhadamente o design arquitetural, o protocolo de hardware e o fluxo de inicialização dos processadores secundários (**Application Processors - APs**) sob coordenação do processador primário (**Bootstrap Processor - BSP**) no **PhotonOS v3.0**.

---

## 1. Visão Geral do Modelo SMP no PhotonOS

O PhotonOS v3.0 implementa o modelo de **Multiprocessamento Simétrico (SMP)** para arquiteturas de 64 bits (`x86_64`). No instante de boot, apenas um único processador é ativado pelo firmware (BIOS/UEFI): o **Bootstrap Processor (BSP)**. Cabe ao BSP inicializar os serviços básicos do sistema de arquivos, console gráfico e memória, para em seguida acordar fisicamente os demais processadores do sistema, denominados **Application Processors (APs)**.

A comunicação de baixo nível para controle de energia e execução dos núcleos é mediada pelo **Local APIC (LAPIC)** através do envio de **Interrupções Interprocessador (IPIs - Inter-Processor Interrupts)**.

```mermaid
sequenceDiagram
    autonumber
    participant BSP as BSP (Bootstrap Processor)
    participant RAM as RAM (0x7000 / Parâmetros)
    participant AP as AP (Application Processor)

    Note over BSP: Executa kernel.c:kmain()
    BSP->>BSP: Desativa PIC 8259 legado
    BSP->>BSP: Inicializa LAPIC (Leitura MSR 0x1B)
    BSP->>RAM: Copia trampoline.bin para 0x7000
    BSP->>BSP: Salva GDT do BSP via sgdt
    
    loop Para cada AP detectado
        BSP->>BSP: Aloca pilha física de 4 KiB (PMM)
        BSP->>RAM: Grava parâmetros (CR3, RSP, ID, Entry Point)
        BSP->>AP: Envia INIT IPI (ICR: 0x00004500)
        Note over BSP: Atraso ~10ms via rdtsc
        BSP->>AP: Envia Startup IPI (ICR: 0x00004607)
        
        activate AP
        Note over AP: Inicia em 16-bit Real Mode (0x7000)
        AP->>AP: Carrega GDT temporária e entra em PM (32-bit)
        AP->>AP: Ativa PAE, carrega CR3 e habilita Long Mode (EFER)
        AP->>AP: Salto longo de 64-bit (long_mode_entry)
        AP->>AP: Inicializa RSP, RDI (ID) e define ready = 1
        deactivate AP
        
        Note over BSP: Monitora flag de pronto a 0x7028 (timeout ~200ms)
        BSP->>RAM: Lê ready == 1?
        
        Note over AP: Salta para ap_kmain()
        activate AP
        AP->>AP: Recarrega GDT oficial via lgdt
        AP->>AP: Executa far-return (lretq) para atualizar CS
        AP->>AP: Ativa APIC local (apic_init_ap)
        AP->>BSP: Adquire smp_lock (exclusão mútua)
        AP->>AP: Incrementa contador global ap_booted_count e emite klog
        AP->>BSP: Libera smp_lock
        AP->>AP: Entra em loop infinito "cli; hlt"
        deactivate AP
    end
```

---

## 2. Fase BSP (Bootstrap Processor)

A fase do BSP compreende toda a preparação do ambiente do kernel e a transmissão dos sinais de controle de hardware para colocar os processadores secundários em execução. Esse fluxo está centralizado nas funções de inicialização do kernel.

### A. Desativação do PIC 8259 Legado
Antes de habilitar o APIC, as linhas de interrupção do controlador herdado **Intel 8259 PIC** devem ser completamente mascaradas para evitar conflitos de barramento e interrupções espúrias. Na função [apic_init()](file:///c:/Users/lucas/OneDrive/Documentos/PhotonOS/src/kernel/apic.c#L33-L52), isso é realizado enviando o valor `0xFF` para as portas de I/O da controladora master (`0x21`) e slave (`0xA1`):

```c
outb(0x21, 0xFF);
io_wait();
outb(0xA1, 0xFF);
io_wait();
```

### B. Mapeamento de Memória do LAPIC
O endereço base físico do Local APIC é dinamicamente determinado através da leitura do registrador de propósito específico **IA32_APIC_BASE MSR** (endereço `0x1B`):

```c
uint64_t apic_base_msr = rdmsr(0x1B);
lapic_base_addr = apic_base_msr & 0xFFFFF000ULL;
```

A base do LAPIC (tipicamente `0xFEE00000`) é mapeada nas tabelas de páginas do kernel com flags de paginação restritivas: `PAGE_PRESENT | PAGE_WRITABLE | PAGE_CACHE_DISABLE` para assegurar acesso direto sem cache (*Memory-Mapped I/O*).

### C. Alocação do Trampolim
Os processadores x86_64, ao receberem o sinal de inicialização de hardware, iniciam obrigatoriamente no modo real de 16 bits. A especificação de boot do processador exige que o ponto de entrada deste código de bootstrap esteja alinhado em uma fronteira de página de 4 KiB nos primeiros 1 MiB de memória física (Região Baixa / *Conventional Memory*).

No PhotonOS v3.0, o endereço físico fixo escolhido é **`0x7000`**. A rotina [smp_init()](file:///c:/Users/lucas/OneDrive/Documentos/PhotonOS/src/kernel/smp.c#L145-L169) copia o blob binário bruto gerado a partir do código assembly [trampoline.asm](file:///c:/Users/lucas/OneDrive/Documentos/PhotonOS/src/kernel/trampoline.asm) para essa região:

```c
extern const uint8_t _binary_trampoline_bin_start[];
extern const uint8_t _binary_trampoline_bin_end[];

void smp_init(void) {
    // ...
    uint64_t size = (uint64_t)(end - src);
    mem_copy((void *)0x7000, _binary_trampoline_bin_start, size);
    __asm__ volatile ("" ::: "memory"); // Barreira de escrita
}
```

### D. Salvamento da GDTR e Estrutura de Handoff
Antes de iniciar os APs, o BSP executa a instrução `sgdt` para salvar os limites e o endereço linear de sua GDT oficial em uma variável global protegida [bsp_gdtr](file:///c:/Users/lucas/OneDrive/Documentos/PhotonOS/src/kernel/smp.c#L50). Os APs acessarão essa variável para recarregar a GDT correta ao entrarem em modo de 64 bits.

Em seguida, na função [smp_boot_ap()](file:///c:/Users/lucas/OneDrive/Documentos/PhotonOS/src/kernel/smp.c#L175-L261), o BSP aloca uma página física de 4 KiB dedicada para servir como a pilha temporária do AP e escreve os parâmetros de sincronização e handoff diretamente nos offsets reservados no cabeçalho do trampolim:

*   **`0x7010` (`ap_cr3`):** Endereço físico do diretório de páginas do Kernel (PML4) lido do registrador `CR3` do BSP.
*   **`0x7018` (`ap_rsp`):** Endereço virtual do topo da pilha de kernel de 4 KiB recém-alocada pelo PMM (`stack_phys + 4096`).
*   **`0x7020` (`ap_id`):** Identificador lógico do núcleo (APIC ID).
*   **`0x7028` (`ap_ready`):** Flag atômica de sincronização (inicializada em 0).
*   **`0x7030` (`ap_entry`):** Endereço virtual do ponto de entrada C do kernel para os APs ([ap_kmain](file:///c:/Users/lucas/OneDrive/Documentos/PhotonOS/src/kernel/smp.c#L270-L340)).

### E. Sequência de Inicialização Física (INIT-SIPI)
A ativação física do núcleo secundário é efetuada escrevendo nos registradores de comando de interrupção (ICR - *Interrupt Command Register*) do LAPIC do BSP:

1.  **Seleção do Alvo:** Escreve-se o APIC ID correspondente deslocado na parte alta do registrador de comando (`APIC_REG_ICR_HIGH`):
    ```c
    apic_write(LAPIC_ICR_HIGH, (uint32_t)ap_id << 24);
    ```
2.  **INIT IPI:** Envia-se o comando `INIT IPI` (valor `0x00004500`) na parte baixa do registrador (`APIC_REG_ICR_LOW`). Este sinal força o AP a resetar seu estado interno de registradores e aguardar o Startup IPI.
3.  **Atraso de Calibração:** Um atraso obrigatório de aproximadamente 10 milissegundos é executado. Como o scheduler ou temporizadores de interrupção do sistema ainda não estão sincronizados para múltiplos núcleos, a espera é calibrada por meio de ciclos de máquina lidos via [rdtsc()](file:///c:/Users/lucas/OneDrive/Documentos/PhotonOS/src/kernel/smp.c#L118-L123).
4.  **Startup IPI (SIPI):** Envia-se o comando de inicialização física contendo o vetor correspondente à página do trampolim. O vetor é calculado dividindo o endereço físico por `4096` (`0x7000 / 0x1000 = 0x07`). Portanto, o comando gravado em `APIC_REG_ICR_LOW` é `0x00004607` (SIPI com vetor `0x07`).
5.  **Acompanhamento e Repetição:** O BSP entra em um loop de leitura de baixa latência monitorando a flag `ready` na memória RAM física (`0x7028`). Se o AP não responder em cerca de 200 ms, o BSP dispara um segundo sinal de SIPI de acordo com as especificações de tolerância a falhas do manual da Intel (SDM). Caso a flag permaneça em zero, o núcleo é ignorado e um erro é registrado no log.

---

## 3. Fase AP (Application Processor)

O processador secundário executa um fluxo de bootstrap dividido em etapas em código Assembly de baixo nível, seguido por uma fase de configuração lógica de registradores em linguagem C (Ring 0).

### A. Execução em Modo Real de 16 bits (`0x7000`)
Ao receber o Startup IPI, o AP acorda com o par de registradores de segmento de código `CS:IP` definidos como `0x0700:0x0000` (apontando diretamente para o início da página física `0x7000`).

1.  **Limpeza de Estado:** O código desativa imediatamente interrupções (`cli`), limpa a flag de direção (`cld`) e zera todos os registradores de segmento clássicos (`DS`, `ES`, `SS`, `FS`, `GS`). Um salto far é executado para redefinir `CS` para `0x0000` absoluto e estabilizar o endereçamento relativo.
2.  **Carregamento de GDT Temporária:** O processador carrega uma GDT básica temporária de 32-bit e 64-bit definida no próprio trampolim ([gdt_ptr](file:///c:/Users/lucas/OneDrive/Documentos/PhotonOS/src/kernel/trampoline.asm#L133-L135)).
3.  **Modo Protegido (32-bit):** Define o bit 0 (PE - *Protection Enable*) do registrador `CR0` para 1, ativando o Modo Protegido e executando um salto far de 32 bits (`jmp dword 8:protected_entry`) para esvaziar a fila de decodificação interna do processador.

### B. Transição de 32-bit Protected Mode para 64-bit Long Mode
Na rotina de 32 bits (`protected_entry`), o AP executa a subida de nível e inicialização da paginação:

1.  **Paginação PAE:** Habilita a extensão de endereço físico (PAE) definindo o bit 5 no registrador de controle `CR4`:
    ```assembly
    mov eax, cr4
    or eax, 1 << 5
    mov cr4, eax
    ```
2.  **Diretório de Páginas (CR3):** O AP lê o endereço do diretório de páginas do Kernel do offset seguro do trampolim (`[0x7010]`) e grava diretamente no registrador `CR3`:
    ```assembly
    mov eax, [0x7010]
    mov cr3, eax
    ```
3.  **Long Mode Enable:** Escreve no registrador de controle de MSR de Modo Longo (`IA32_EFER` em `0xC0000080`), ativando o bit 8 (LME - *Long Mode Enable*):
    ```assembly
    mov ecx, 0xC0000080
    rdmsr
    or eax, 1 << 8
    wrmsr
    ```
4.  **Ativação de Paginação:** Define o bit 31 (PG) e o bit 0 (PE) no registrador `CR0` simultaneamente para ativar a paginação de 4 níveis de 64 bits.
5.  **Salto para Modo Longo:** Executa um salto longo usando o seletor de segmento de código de 64 bits (`0x18` correspondente à posição na GDT temporária) para entrar em `long_mode_entry`.

### C. Modo Longo de 64 bits (`long_mode_entry`)
Executando nativamente em 64 bits, o AP assume sua própria infraestrutura de memória:

1.  **Pilha Dedicada:** Carrega o registrador de pilha `RSP` a partir do offset `0x7018` configurado pelo BSP e aplica alinhamento de 16 bytes em conformidade com as diretivas de chamada da ABI de 64 bits.
2.  **Argumentos da Função:** Copia o ID lógico do núcleo gravado em `[0x7020]` para o registrador `RDI` (primeiro argumento da convenção do sistema).
3.  **Sinalização de Conclusão:** Define o valor no offset `0x7028` (`ap_ready`) para 1 na memória RAM física. Isso encerra o loop de espera do BSP.
4.  **Ponto de Entrada C:** Efetua um salto indireto para a função de kernel em C armazenada no offset `0x7030` (`ap_entry`):
    ```assembly
    mov rax, [0x7030]
    jmp rax
```

### D. Execução do Ponto de Entrada C (`ap_kmain`)
Após saltar do trampolim de assembly, o AP executa a rotina [ap_kmain()](file:///c:/Users/lucas/OneDrive/Documentos/PhotonOS/src/kernel/smp.c#L270-L340) em Ring 0:

1.  **GDT do Kernel:** A GDT básica usada no trampolim serviu apenas para a transição. O AP agora lê a variável global `bsp_gdtr` e recarrega os limites e base da GDT oficial do kernel:
    ```c
    __asm__ volatile ("lgdt %0" :: "m"(bsp_gdtr));
    ```
2.  **Far-Return (lretq):** Para atualizar o seletor do registrador de segmento de código (`CS`) de modo que aponte para a entrada `0x08` da nova GDT oficial do kernel, o AP executa um far-return manual empilhando o seletor `0x08` e o endereço linear de continuação, aplicando a instrução `lretq`:
    ```c
    __asm__ volatile (
        "pushq $0x08\n\t"
        "leaq 1f(%%rip), %%rax\n\t"
        "pushq %%rax\n\t"
        "lretq\n\t"
        "1:\n\t"
        ::: "rax", "memory"
    );
    ```
3.  **Segmentos de Dados:** Recarrega os registradores de segmento de dados (`DS`, `ES`, `SS`) com o seletor de dados do kernel (`0x10`).
4.  **Ativação do LAPIC Local:** O processador ativa o Local APIC de sua própria CPU definindo o bit 8 do registrador de vetor de interrupção espúria (SIVR - *Spurious Interrupt Vector Register* em `0xF0`) para habilitar o tratamento local de interrupções físicas e lógicas:
    ```c
    apic_init_ap();
    ```
5.  **Exclusão Mútua de Log:** Para evitar corrupções ou intercalamento de strings no logger de barramento serial (uma vez que múltiplos APs podem estar entrando neste ponto concorrentemente), o AP adquire a primitiva atômica `smp_lock` (Spinlock baseado em `__sync_lock_test_and_set`). Sob posse do lock, registra a mensagem de sucesso no `klog`, incrementa o contador global de núcleos ativos e libera o lock.
6.  **Repouso Seguro (Idle Loop):** Uma vez inicializado, o AP desativa interrupções de hardware locais e entra em estado de economia de energia através da instrução de interrupção e parada do processador (`cli; hlt`):
    ```c
    for (;;) {
        __asm__ volatile ("cli; hlt" ::: "memory");
    }
    ```
    Este núcleo permanecerá em repouso absoluto até que o escalonador multiprocessador do kernel atribua a ele tarefas ou filas de execução locais em Ring 3.
