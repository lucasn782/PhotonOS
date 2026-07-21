# Transmission Control Protocol (TCP)

Este documento especifica o suporte atual ao protocolo TCP (Transmission Control Protocol) no kernel do PhotonOS, detalhando o mecanismo implementado de estabelecimento de conexão (Three-Way Handshake) e suas severas limitações atuais de transmissão de dados.

---

## 1. Estado de Implementação: Conexão Apenas (Three-Way Handshake)

No PhotonOS v4.1, o protocolo TCP **não** possui uma implementação completa. Ele consiste em uma **prova de conceito funcional para o handshake de conexão**, permitindo transicionar sockets de fluxo (`SOCK_STREAM`) para o estado `ESTABLISHED`. Ele é incapaz de transmitir ou receber payloads de dados.

---

## 2. Estrutura do Cabeçalho TCP

O cabeçalho TCP é mapeado no arquivo `src/kernel/net.c` pela estrutura `struct tcp_header`:

```c
struct tcp_header {
    uint16_t src_port;            // Porta de origem
    uint16_t dest_port;           // Porta de destino
    uint32_t seq_num;             // Número de sequência
    uint32_t ack_num;             // Número de confirmação
    uint16_t data_offset_flags;   // Offset de dados (4 bits) + Reservado (6 bits) + Flags (6 bits)
    uint16_t window_size;         // Tamanho da janela
    uint16_t checksum;            // Checksum cobrindo pseudo-cabeçalho IP + cabeçalho
    uint16_t urgent_ptr;          // Ponteiro de urgência
} __attribute__((packed));
```

### Flags de Controle Suportadas
*   `TCP_FLAG_SYN` (`0x02U`): Usada para sincronizar números de sequência na inicialização da conexão.
*   `TCP_FLAG_ACK` (`0x10U`): Usada para confirmar dados ou handshakes recebidos.
*   `TCP_FLAG_RST` (`0x04U`): Usada para abortar ou rejeitar conexões.
*   `TCP_FLAG_FIN` (`0x01U`): Definida na macro, mas **não utilizada** no fluxo de fechamento.

---

## 3. Protocolo de Conexão (Three-Way Handshake)

Quando um processo de Ring 3 cria um socket do tipo `SOCK_STREAM` e invoca `sys_connect` (Syscall 26):

```
       PhotonOS (Client)                           Host Remoto (Server)
               │                                            │
   [state = TCP_SYN_SENT]                                   │
               │─────── Envia SYN (seq = ISS) ─────────────►│
               │                                            │
               │◄────── Recebe SYN-ACK ─────────────────────│ (seq_ack == ISS + 1)
   [state = TCP_ESTABLISHED]                                │
               │─────── Envia ACK (ack = rcv_seq + 1) ─────►│
               ▼                                            ▼
```

### Fluxo Detalhado
1.  **Alocação do TCB**: O kernel aloca um Bloco de Controle TCP (`struct tcb`) contendo a sequência inicial de envio (`iss`) baseada em `kernel_ticks`, números de sequência de controle (`snd_una`, `snd_nxt`), próximo número de sequência esperado (`rcv_nxt`) e a janela padrão (`rcv_wnd = 65535`).
2.  **Transmissão do SYN**: O socket entra no estado `TCP_SYN_SENT`. O kernel despacha um pacote contendo apenas o cabeçalho TCP com a flag `TCP_FLAG_SYN` ativa e o número de sequência inicial (`iss`).
3.  **Bloqueio de Tarefa**: A tarefa que invocou `connect` é bloqueada no escalonador com o motivo `TASK_WAIT_NETWORK` e cede a CPU.
4.  **Resposta da Rede**:
    *   Quando a thread de rede recebe um pacote com protocolo IP `6` (TCP) e as flags `SYN` + `ACK` ativas, ela valida se o ACK confirma o SYN enviado (`seg_ack == s->tcb->snd_nxt`).
    *   O socket transiciona para `TCP_ESTABLISHED`. Os contadores são atualizados (`rcv_nxt = seg_seq + 1`, `snd_una = seg_ack`).
    *   O kernel envia de volta um segmento contendo a flag `ACK` (confirmando o SYN do servidor).
    *   A tarefa de usuário é acordada via `scheduler_wake_socket`.

---

## 4. Limitações Atuais e Débitos Técnicos (Não Implementado)

> [!WARNING]
> **O subsistema TCP possui as seguintes lacunas de desenvolvimento:**

1.  **Sem Transmissão de Dados (`socket_vfs_write`)**: A chamada `write()` sobre um socket do tipo `SOCK_STREAM` retorna `0` sem realizar nenhum envio de dados pela rede física.
2.  **Sem Recepção de Dados (`socket_vfs_read`)**: O kernel não possui tratadores para segments TCP de dados recebidos no loop principal. Caso um pacote contendo payload chegue, ele não é processado e a chamada `read()` do usuário ficará bloqueada indefinidamente.
3.  **Fechamento Incompleto (`socket_vfs_close`)**: A rotina de fechamento de socket libera a memória das filas e o TCB local, mas **não** executa o protocolo de desconexão padrão de 4 etapas (segmento `FIN` → `ACK` → `FIN` → `ACK`). A conexão é simplesmente destruída no lado do cliente.
4.  **Falta de Retransmissão**: O kernel não implementa timeouts (RTO) ou retransmissão de pacotes perdidos caso o handshake falhe.
