# Internet Control Message Protocol (ICMP)

Este documento detalha a especificação e a implementação do protocolo ICMP (Internet Control Message Protocol) no kernel do PhotonOS, especificamente focado no suporte a operações de Echo Request (Ping) e Echo Reply.

---

## 1. Visão Geral do ICMP no PhotonOS

O suporte a ICMP no PhotonOS está integrado à pilha de rede do kernel (`src/kernel/net.c`). O objetivo primário da implementação é responder automaticamente a requisições de **Ping (Echo Request)** enviadas por hosts remotos para validar a conectividade e latência do sistema, além de permitir o envio de pacotes de ping a partir do espaço de usuário via sockets brutos (`SOCK_RAW`).

---

## 2. Estrutura do Pacote ICMP

O cabeçalho do pacote ICMP é mapeado no arquivo `include/net.h`:

```c
struct __attribute__((packed)) icmp_packet {
    uint8_t type;       // Tipo de mensagem ICMP
    uint8_t code;       // Código da mensagem (específico do tipo)
    uint16_t checksum;  // Checksum do pacote ICMP (1's complement)
    uint16_t id;        // Identificador (usado no Echo Request/Reply)
    uint16_t sequence;  // Número de sequência (usado no Echo Request/Reply)
    uint8_t data[];     // Payload opcional
};
```

### Tipos Suportados
*   `ICMP_TYPE_ECHO_REQ` (`8`): Requisição de eco (enviada por outros hosts ou pelo utilitário `ping`).
*   `ICMP_TYPE_ECHO_REPLY` (`0`): Resposta de eco (enviada pelo kernel em resposta a um Request, ou recebida e enfileirada no socket do usuário).

---

## 3. Fluxo de Tratamento de Echo Request (`net_handle_icmp`)

Quando a placa e1000 recebe um pacote IP cuja flag de protocolo indica `IP_PROTO_ICMP` (`1`), a rotina `net_poll_packets` valida o checksum do pacote ICMP e invoca `net_handle_icmp`:

```
[Rede Física] ──► [Driver e1000] ──► [net_poll_packets]
                                            │ (Valida Checksum e Proto=1)
                                            ▼
                                    [net_handle_icmp]
                                            │
                                  (Verifica Type == 8?)
                                            │ SIM
                                            ▼
                               [Envia ICMP Echo Reply]
```

O fluxo interno de `net_handle_icmp` realiza as seguintes operações:
1.  **Validação dos Parâmetros**: Garante que o pacote é IPv4, com destino direcionado ao IP local (`0x0A00020F` / `10.0.2.15`), e que o tipo ICMP é `ICMP_TYPE_ECHO_REQ` (`8`) com código `0`.
2.  **Inversão de Endereços**:
    *   No cabeçalho Ethernet, o MAC de origem (`src_mac`) do pacote recebido é copiado para o MAC de destino (`dest_mac`), e o MAC de origem é preenchido com o MAC local.
    *   No cabeçalho IPv4, o IP de origem (`src_ip`) torna-se o IP de destino, e o IP local é definido como a origem.
3.  **Ajuste do Tipo ICMP**: O tipo ICMP é alterado de `8` (Echo Request) para `0` (Echo Reply).
4.  **Recálculo de Checksums**:
    *   O checksum do cabeçalho IP é zerado e recalculado usando `net_checksum`.
    *   O checksum do pacote ICMP (incluindo o payload original de dados) é zerado e recalculado.
5.  **Transmissão**: O pacote modificado é imediatamente despachado de volta para a rede via driver `e1000_send_packet` a partir do mesmo buffer, minimizando alocações de memória.

---

## 4. Integração com Sockets Brutos (`SOCK_RAW`)

Para permitir que utilitários do espaço de usuário enviem e recebam pings:
*   **Envio**: O programa cria um socket do tipo `SOCK_RAW` com protocolo `IP_PROTO_ICMP`. Ao chamar `write` ou `socket_send`, o kernel encapsula o payload fornecido pelo usuário (que já deve conter o cabeçalho ICMP completo) e despacha via `net_send_ipv4`.
*   **Recepção**: Quando um pacote ICMP com IP de destino local é recebido e validado por `net_poll_packets`, o kernel verifica a tabela de sockets ativos. Se houver um socket correspondente a `SOCK_RAW` com protocolo `IP_PROTO_ICMP`, o payload IP (cabeçalho ICMP + dados) é copiado para o buffer de recepção (`rx_queue`) do socket, e a tarefa de usuário correspondente é acordada via `scheduler_wake_socket`.

---

## 5. Limitações Atuais

*   **Validação de ID**: O kernel responde a qualquer Echo Request destinado a ele sem validar ou rastrear IDs.
*   **Falta de Mensagens de Erro**: O kernel do PhotonOS não gera mensagens ICMP de erro (como *Destination Unreachable* ou *Time Exceeded*) quando pacotes IP falham em ser entregues.
