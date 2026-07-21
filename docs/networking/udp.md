# User Datagram Protocol (UDP)

Este documento descreve o suporte e a especificação do protocolo UDP (User Datagram Protocol) no kernel do PhotonOS, cobrindo o design de sockets, transmissão, recepção e validações de checksum.

---

## 1. Visão Geral do UDP no PhotonOS

O PhotonOS fornece suporte nativo a datagramas UDP em Ring 0 (`src/kernel/net.c`), permitindo comunicações não-confiáveis e sem conexão entre processos usuários e redes externas. Os processos interagem com o protocolo através da interface de sockets BSD padrão usando a chamada de sistema `sys_socket` com o tipo `SOCK_DGRAM` e protocolo `IP_PROTO_UDP` (`17`).

---

## 2. Estrutura do Cabeçalho UDP

O cabeçalho UDP tem tamanho fixo de 8 bytes, mapeado pela estrutura `struct udp_header` em `src/kernel/net.c`:

```c
struct udp_header {
    uint16_t src_port;   // Porta de origem (network byte order)
    uint16_t dest_port;  // Porta de destino (network byte order)
    uint16_t length;     // Comprimento total do cabeçalho + payload
    uint16_t checksum;   // Checksum cobrindo pseudo-cabeçalho IP, cabeçalho e dados
} __attribute__((packed));
```

---

## 3. Transmissão de Datagramas (`net_send_udp`)

O fluxo de envio de dados UDP segue as seguintes etapas:
1.  **Alocação de Buffer**: É alocado um buffer contíguo em RAM (`kmalloc`) de tamanho `sizeof(struct udp_header) + payload_len`.
2.  **Montagem do Cabeçalho**: A porta de origem e a porta de destino são convertidas para a ordenação de bytes da rede (`htons`).
3.  **Cópia de Dados**: O payload de usuário é copiado para a região posterior ao cabeçalho.
4.  **Cálculo do Checksum**: O kernel calcula o checksum UDP obrigatório (`net_calculate_udp_checksum`) que protege a integridade dos dados e do cabeçalho, simulando um pseudo-cabeçalho IP:
    *   IP de Origem (IP local) e IP de Destino.
    *   Protocolo (`17`).
    *   Comprimento do cabeçalho + payload UDP.
5.  **Encapsulamento e Envio**: O datagrama montado é enviado via `net_send_ipv4` com o protocolo `IP_PROTO_UDP` (`17`). Ao final, o buffer temporário é desalocado via `kfree`.

---

## 4. Recepção e Enfileiramento de Datagramas

Quando a placa Ethernet recebe um pacote IPv4 com protocolo `17` (UDP), a rotina `net_poll_packets` intercepta o fluxo:
1.  **Validação de Integridade**: O checksum UDP é validado via `net_validate_udp_checksum`. Se for inválido, o pacote é imediatamente descartado com aviso de log (`Checksum UDP invalido`).
2.  **Busca por Socket Ativo**: O kernel percorre a tabela global de sockets (`sockets[16]`) procurando por uma entrada ativa que atenda aos critérios:
    *   `s->active == 1`
    *   `s->type == SOCK_DGRAM`
    *   `s->protocol == IP_PROTO_UDP`
    *   `s->local_port == dest_port` (a porta local do socket é igual à porta de destino do pacote).
3.  **Enfileiramento**:
    *   Se um socket correspondente for encontrado e sua fila de entrada (`rx_count`) estiver abaixo do limite (`SOCKET_RX_BUF_SIZE = 16`), o payload UDP é copiado para um buffer alocado dinamicamente (`kmalloc`).
    *   O pacote é enfileirado na posição `rx_tail` do socket, e o ponteiro `rx_tail` é incrementado de forma circular.
    *   A tabela registra o IP e a porta de origem do remetente remoto em `remote_addr` e `remote_port`.
4.  **Notificação e Preempção**: O kernel aciona `scheduler_wake_socket(target_sock)` e `scheduler_wake_socket_receivers(IP_PROTO_UDP)` para acordar qualquer tarefa que esteja bloqueada em uma syscall de leitura sobre este socket.

---

## 5. Limitações Atuais

*   **Tamanho Máximo do Datagrama**: O payload UDP é limitado a `1480` bytes devido ao MTU padrão e à ausência de fragmentação IP no kernel.
*   **Ausência de Descarte Silencioso ICMP**: Caso um pacote chegue para uma porta UDP sem sockets associados, o kernel apenas loga o descarte em `klog` em vez de responder com uma mensagem ICMP *Port Unreachable*.
*   **Fila Limitada**: A fila de recepção estática de 16 datagramas pode descartar dados sob tráfego severo.
