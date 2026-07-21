# Linear Framebuffer (VBE Graphics Driver)

Este documento detalha o design, o pipeline de renderização gráfica e as rotinas de sincronização por software do Linear Framebuffer (LFB) baseadas nas extensões VBE (VESA BIOS Extensions) no PhotonOS.

---

## 1. Configuração e Inicialização Gráfica

A placa de vídeo é inicializada em modo gráfico pelo bootloader (`src/boot/boot.asm`) durante a inicialização do sistema em Modo Real. O bootloader:
1.  Busca as capacidades gráficas usando chamadas de BIOS `INT 0x10, AX=0x4F00`.
2.  Interroga a geometria do modo de vídeo **1024x768 pixels True Color (32 bits por pixel)** (Modo `0x4118`).
3.  Salva os dados físicos retornados na estrutura global de parâmetros de inicialização (`boot_params`):
    *   **`phys_base_ptr`**: Endereço base físico do Linear Framebuffer.
    *   **`width` / `height`**: Dimensões da tela (1024 / 768).
    *   **`pitch`**: Quantidade de bytes de uma linha horizontal gráfica (stride).

---

## 2. Mapeamento de Memória Virtual e Caching

No subsistema de memória virtual (VMM), a memória física do monitor é mapeada em alta memória no kernel:

```c
#define LFB_VIRTUAL_BASE 0xFFFFFFFFC0000000ULL
```

### Configurações de Cache (PTE Flags)
Para garantir desempenho máximo de redesenho e evitar que leituras/escritas concorrentes da CPU causem artefatos visuais:
*   `VMM_PAGE_CACHE_DISABLE` (`CD`): Desativa o cache L1/L2 para escritas na memória de vídeo, garantindo que pixels alterados sejam transmitidos imediatamente à GPU.
*   `VMM_PAGE_WRITE_THROUGH` (`WT`): Força a CPU a gravar qualquer pixel alterado de forma síncrona através do barramento, contornando buffers de escrita atrasados.

---

## 3. Deferred Buffering (Double Buffering)

Devido à alta latência do barramento PCI MMIO no acesso a pixels individuais da tela física, o PhotonOS utiliza uma técnica de **Deferred Buffering (Double Buffering)**.

O kernel mantém uma cópia completa dos pixels do monitor em memória RAM convencional (muito mais rápida para escritas individuais):

```c
#define BACKBUFFER_VIRTUAL_BASE 0xFFFFFFFFC1000000ULL
```

Todas as operações de desenho do kernel (`video_put_pixel`, `video_draw_char`, `video_clear`, `video_fill_rect`) modificam apenas este **backbuffer**. A atualização da tela física do monitor é diferida e ocorre de uma única vez em `video_swap_buffers()`.

```
Operações de Desenho (Ring 0)
    │
    ▼
[Backbuffer (RAM)] ──(video_swap_buffers)──► [LFB Físico (Vídeo)]
```

A cópia do backbuffer para a tela física é otimizada realizando transferências em blocos de 64 bits (`uint64_t`), o que satura a largura de banda de memória disponível.

---

## 4. Elementos de Interface por Software e Cursores

Como não há suporte para cursor de hardware na emulação de VBE básica, o PhotonOS gerencia o cursor do mouse e o cursor de texto do terminal inteiramente via software durante a sincronização de buffers em `video_swap_buffers()`:

1.  **Salvamento de Pixels Subjacentes**:
    *   Antes de desenhar os cursores, o kernel lê a região sob os cursores no backbuffer e armazena os pixels nos arrays temporários `saved_pixels` (mouse $19 \times 12$ pixels) e `saved_text_cursor` (bloco de cursor de texto $8 \times 16$ pixels).
2.  **Desenho dos Sprites**:
    *   **Cursor de Texto**: Renderiza um bloco retangular sólido branco no backbuffer na coordenada da linha/coluna atual do terminal.
    *   **Sprite do Mouse**: Renderiza um cursor em formato de seta assimétrica branca com contorno preto no backbuffer.
3.  **Sincronização no LFB**: O backbuffer com os cursores sobrepostos é copiado na tela física do monitor.
4.  **Restauração**: Imediatamente após a cópia física, os pixels armazenados em `saved_pixels` e `saved_text_cursor` são reescritos no backbuffer. Isso remove as imagens dos cursores do backbuffer da RAM. Assim, na próxima iteração do ciclo de desenho, quando o mouse se mover, ele não deixará rastros ou borrões na tela.

---

## 5. Sincronização SMP (Locking)

Para evitar que múltiplos núcleos da CPU concorram pelo desenho visual em ambientes multicore, as operações de desenho e o swap de buffers são protegidos pelo spinlock global `video_lock`:

```c
static spinlock_t video_lock;

void video_put_pixel(int x, int y, uint32_t color) {
    // ...
    uint64_t flags = spin_lock_irqsave(&video_lock);
    backbuffer[y * stride + x] = color;
    spin_unlock_irqrestore(&video_lock, flags);
}
```

Isso garante atomicidade gráfica e previne a corrupção visual das estruturas de interface de vídeo.
