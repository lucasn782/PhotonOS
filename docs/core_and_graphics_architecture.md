# Arquitetura Core e Subsistema Gráfico por Software (v2.0)

Este documento detalha as especificações técnicas da arquitetura do núcleo (Ring 0 / Long Mode de 64-bits) e o subsistema de vídeo do PhotonOS v2.0, cobrindo o gerenciamento do descritor da GDT, isolamento de exceções com a TSS, o pipeline gráfico VBE e elementos de interface renderizados por software.

---

## 1. Arquitetura Core & Modo de Operação (64-bit Long Mode)

O PhotonOS v2.0 opera nativamente em Modo Longo de 64-bits. A transição e a estabilização deste modo exigem alinhamento rigoroso das tabelas de descritores de hardware do processador x86_64.

### A. Descritor da GDT (Global Descriptor Table) e Alinhamento
No modo longo de 64 bits, o registrador da tabela de descritores globais (GDTR) exige um limite de 16 bits seguido por um endereço base de 64 bits completo.
*   **Correção de Truncamento:** Nas versões anteriores, a base do descritor da GDT era declarada incorretamente usando diretivas de 32 bits (`dd`), o que truncava os bits superiores do endereço de carregamento e causava falhas silenciosas de paginação em carregamentos de páginas acima de 4GB.
*   **Implementação Consolidada:** O descritor foi redefinido utilizando a diretiva de 64 bits (`dq`) para carregar a base linear completa da GDT:
    ```assembly
    gdt_descriptor:
        dw gdt_end - gdt_start - 1  ; Limite (16 bits)
        dq gdt_start                ; Endereço Base de 64 bits
    ```
    Isso assegura que a base de 64 bits seja integralmente lida pelo hardware na instrução `lgdt [gdt_descriptor]`, estabilizando o endereçamento virtual do kernel em alta memória (*higher-half*).

### B. TSS (Task State Segment) e Mitigação de Falhas (IST1)
Sob condições normais, falhas graves no kernel (como estouro de pilha) corrompem a pilha ativa do kernel (`rsp`). Quando o processador tenta empilhar o frame da exceção na mesma pilha corrompida, ocorre uma falha secundária. Se essa falha não puder ser entregue, o hardware dispara um reset instantâneo (Triple Fault).

Para impedir que falhas de Ring 0 provoquem resets de hardware, o PhotonOS v2.0 utiliza a infraestrutura de tabelas de pilhas de interrupção (IST) do Task State Segment de 64 bits:
1.  **Alocação da Pilha Isolada:** É alocada uma área de pilha exclusiva e estática para Double Faults:
    ```c
    static uint8_t double_fault_stack[4096] __attribute__((aligned(4096)));
    ```
2.  **Configuração do IST1 na TSS:** O primeiro slot de pilha de interrupção (IST1) da TSS é carregado com o topo desta pilha isolada durante a inicialização (`tss_init`):
    ```c
    kernel_tss.ist1 = (uint64_t)&double_fault_stack[4096];
    ```
3.  **Encadeamento na IDT:** A entrada correspondente à interrupção de Double Fault (`INT 0x08`) na IDT é configurada explicitamente para utilizar a tabela de pilha de interrupção 1 (IST=1):
    ```c
    idt_set_gate(8, double_fault_stub);
    idt[8].ist = 1; // Associa a exceção 8 ao IST1 configurado na TSS
    ```
Quando ocorre um Double Fault, o processador x86_64 intercepta a falha, lê a TSS para carregar a pilha isolada de `ist1` em `rsp`, empilha com segurança o frame de erro e executa `double_fault_handler()`. Isso possibilita a emissão de um relatório detalhado de Kernel Panic (com dump de RIP, RSP, Error Code e registradores de controle CR0-CR4) em vez de reiniciar a máquina.

---

## 2. Subsistema Gráfico, Terminal e UI por Software

O PhotonOS v2.0 adota um pipeline gráfico de alto desempenho renderizado inteiramente por software, dispensando adaptadores VGA legados em modo texto em favor do Vesa BIOS Extensions (VBE) linear.

### A. Pipeline Gráfico VBE e LFB
*   **Inicialização do Modo Gráfico:** O bootloader configura o hardware de vídeo em modo linear VBE na resolução de **1024x768 pixels com 32 bits de profundidade de cor** (True Color).
*   **Mapeamento de Alta Memória:** O Linear Framebuffer (LFB) mapeia o endereço físico da placa de vídeo (`phys_base_ptr`) no endereço virtual superior estável do Kernel:
    ```c
    #define LFB_VIRTUAL_BASE 0xFFFFFFFFC0000000ULL
    ```
*   **Flags de Paginação e Sincronização:** Para maximizar o rendimento e evitar discrepâncias visuais, as páginas do LFB são mapeadas no VMM com flags de cache estritas:
    *   `VMM_PAGE_CACHE_DISABLE` (CD): Impede que o processador armazene em cache de escrita os pixels do monitor.
    *   `VMM_PAGE_WRITE_THROUGH` (WT): Força a escrita direta de qualquer modificação de pixel diretamente na memória da placa de vídeo, garantindo atualização instantânea da tela.

### B. Console Dinâmico e Deferred Buffering
*   **Grade Adaptativa:** O console baseia-se em uma grade adaptativa em que as dimensões de caracteres são calculadas a partir das dimensões reais e Stride informados pelo hardware VBE. Utilizando uma fonte rasterizada embutida de $8 \times 16$ pixels, as dimensões são dinamicamente calculadas como:
    $$\text{Colunas} = \frac{\text{width}}{8} = 128 \quad\text{e}\quad \text{Linhas} = \frac{\text{height}}{16} = 48$$
*   **Deferred Buffering (Double Buffering):** Para contornar a latência do barramento MMIO PCI nas escritas de pixels individuais, o Kernel mantém um buffer de sombra (backbuffer) mapeado em memória RAM convencional:
    ```c
    #define BACKBUFFER_VIRTUAL_BASE 0xFFFFFFFFC1000000ULL
    ```
    Todas as operações de desenho (`video_put_pixel`, `video_draw_char`, `video_clear`) escrevem no backbuffer. A exibição na tela física é diferida e ocorre de uma só vez na rotina `video_swap_buffers()`.
*   **Cópia de Memória Otimizada:** A transferência do backbuffer para o LFB utiliza blocos alinhados de 64 bits (`uint64_t`) para saturar a largura de banda do barramento de memória, tratando qualquer sobra de bytes de forma segura no final do fluxo.

### C. Elementos de UI por Software (Renderização e Limpeza)
Como não há aceleração de hardware para o cursor do mouse e cursor de texto, o kernel gerencia essas entidades via software de forma embutida no ciclo de swap de buffers.

```
[Backbuffer]  ------(Salva pixels originais)-----> [Saved Buffers]
     │                                                    │
     ├─► Desenha Cursor de Texto (Bloco Branco)           │
     ├─► Desenha Sprite do Mouse (Seta 19x12)             │
     │                                                    │
     ▼                                                    ▼
[LFB (Tela)] <-------(Copia Backbuffer)                  │
     │                                                    │
     └◄---------(Restaura pixels originais)---------------┘
```

1.  **Salvamento de Pixels Subjacentes:** Antes de renderizar os sprites dos cursores no backbuffer, a rotina `video_swap_buffers` lê e armazena os pixels contidos nas caixas delimitadoras do mouse ($19 \times 12$) e do cursor de texto ($8 \times 16$) nos buffers temporários `saved_pixels` e `saved_text_cursor`.
2.  **Desenho dos Cursores:**
    *   **Cursor de Texto:** Desenha um bloco retangular de cor `0x00FFFFFF` nas coordenadas dinâmicas fornecidas pela coluna e linha ativas do terminal.
    *   **Sprite do Mouse:** Desenha um sprite em formato de Seta Angular Simétrica ($19 \times 12$ pixels), contendo contorno preto (`0x00000000`) e preenchimento branco (`0x00FFFFFF`).
3.  **Escrita no LFB:** O backbuffer contendo os elementos de interface gráfica sobrepostos é copiado na íntegra para o LFB físico.
4.  **Restauração Automática:** Imediatamente após a cópia do buffer para o hardware, os pixels salvos em `saved_pixels` e `saved_text_cursor` são reescritos no backbuffer. Isso apaga os cursores do backbuffer da RAM. Assim, na próxima atualização, quando o cursor ou mouse se moverem, eles não deixarão rastros na tela física.
