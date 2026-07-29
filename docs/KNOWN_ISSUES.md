# Problemas Conhecidos (Known Issues)

Este documento cataloga as limitações de design, bugs conhecidos, implementações parciais e restrições de compatibilidade existentes no PhotonOS v4.3-fs.

---

## 1. Subsistema de Rede (Networking)

### 1.1 Incompletude da Pilha TCP
*   **Descrição**: Embora o kernel implemente com sucesso a máquina de estados para estabelecimento de conexões (Three-Way Handshake via `sys_connect` com estados `SYN_SENT` e `ESTABLISHED`), o pipeline de fluxo de dados não está implementado.
*   **Sintomas**: A invocação de `sys_write` / `write()` sobre sockets do tipo `SOCK_STREAM` retorna `0` ou `-1` sem transmitir nenhum segmento de dados na rede. A invocação de `sys_read` / `read()` bloqueia indefinidamente na fila de recepção, pois o kernel não processa payloads TCP entrantes.
*   **Mitigação**: Para transferências funcionais atuais, deve-se utilizar sockets UDP (`SOCK_DGRAM`) ou raw sockets (`SOCK_RAW`).

### 1.2 Ausência de Mensagens de Erro ICMP (Port Unreachable)
*   **Descrição**: Caso um datagrama UDP chegue para uma porta local de destino na qual não haja nenhum socket ativo vinculado (`bind`), o kernel descarta o pacote silenciosamente e registra uma mensagem de log (`UDP: no socket for port...`).
*   **Sintomas**: O host remetente não recebe nenhum aviso de erro, violando a especificação RFC 792, que determina o envio de um pacote ICMP *Destination Unreachable - Port Unreachable* (Tipo 3, Código 3).

---

## 2. Sistemas de Arquivos (Filesystem)

### 2.1 Limitação de Escrita FAT16 no Diretório Raiz
*   **Descrição**: O driver de escrita do FAT16 (`src/drivers/fat16.c`) suporta a criação e escrita de arquivos apenas no diretório raiz (`/`).
*   **Sintomas**: Tentativas de criar ou gravar arquivos dentro de subdiretórios (ex: `/bin/novo_arquivo`) falham ou retornam erros de VFS. O suporte a subdiretórios no FAT16 é somente leitura.

### 2.2 Ausência de Suporte a Nomes Longos (LFN)
*   **Descrição**: O driver FAT16 processa apenas a assinatura clássica de diretório 8.3 (8 caracteres para nome, 3 para extensão).
*   **Sintomas**: Nomes de arquivos criados com mais de 8 caracteres ou extensões com mais de 3 caracteres são truncados ou ignorados pelo kernel, impedindo a compatibilidade com volumes modernos.

---

## 3. Concorrência e Multiprocessamento (SMP)

### 3.1 Ausência de Watchdog/Timeout no TLB Shootdown
*   **Descrição**: Durante a clonagem de espaço de endereçamento (`vmm_clone_address_space`), o núcleo BSP dispara uma IPI (Vector `0x79`) e entra em um loop de espera ativa esperando que todos os outros processadores ativos validem a invalidação do TLB:
    ```c
    while (tlb_acknowledge_count < active_aps) {
        __asm__ volatile ("pause");
    }
    ```
*   **Sintomas**: Se um núcleo AP secundário travar por qualquer falha de hardware ou se encontrar com interrupções mascaradas (`cli`) de forma persistente, o BSP entrará em **deadlock permanente**, travando a execução de todo o sistema operacional. Não há temporizador ou watchdog com timeout para abortar a espera.

### 3.2 Concorrência no Buffer Circular do Teclado
*   **Descrição**: O ring buffer circular do driver de teclado PS/2 (`keyboard_queue`) é acessado de forma assíncrona pelo tratador de interrupção IRQ 1 e pelas threads que leem o console.
*   **Sintomas**: Sob taxas de digitação extremamente rápidas em cenários multi-core, a falta de sincronização via spinlocks nos índices de escrita (`keyboard_queue_write`) e leitura (`keyboard_queue_read`) pode, teoricamente, causar condições de corrida e perda ou repetição de caracteres.

---

## 4. Interface Gráfica e Vídeo

### 4.1 Falta de Sincronização Vertical (V-Sync)
*   **Descrição**: A cópia do backbuffer na memória RAM física para o Linear Framebuffer da placa de vídeo em `video_swap_buffers()` ocorre sequencialmente sem sincronização com o ciclo de atualização vertical do monitor (V-Sync).
*   **Sintomas**: Sob taxas intensivas de redesenho gráfico (ex: movimentação brusca do cursor do mouse), pode ocorrer o efeito de "rasgo" na imagem (screen tearing).

---

## 5. Limitações de Recursos e Limites Estáticos

*   **Tabela de Processos Limitada**: O escalonador possui um limite estático rígido de `16` tarefas simultâneas (`MAX_TASKS = 16`). Uma vez atingido o limite, qualquer chamada `fork` ou `spawn` falhará até que tarefas zumbis sejam reclamadas via `wait`.
*   **Tabela de Sockets Estática**: O subsistema de rede possui suporte a no máximo `16` sockets ativos simultâneos (`sockets[16]`).
*   **Fila de Entrada Sockets Limitada**: Cada socket possui um buffer circular estático de no máximo 16 pacotes na fila de recepção (`SOCKET_RX_BUF_SIZE = 16`). Sob tráfego pesado de rede, datagramas excedentes são descartados silenciosamente.
