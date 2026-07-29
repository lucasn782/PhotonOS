# 🔄 Máquina de Estados TCP (Fase 1) — PhotonOS v4.2

## 🌐 Visão Geral

O **PhotonOS v4.2** implementa a infraestrutura da máquina de estados do protocolo TCP conforme definida na **RFC 793**. Nesta primeira fase, são suportados os 5 estados fundamentais necessários para abertura passiva/ativa e estabelecimento de conexão orientada a fluxo.

---

## 📊 Diagrama de Transição de Estados (Fase 1)

```text
                     +-----------------------+
                     |        CLOSED         |
                     +-----------------------+
                       /                   \
        Active Open   /                     \ Passive Open
      send SYN       /                       \ listen
                    v                         v
          +------------------+       +------------------+
          |     SYN_SENT     |       |      LISTEN      |
          +------------------+       +------------------+
                    |                         |
                    | recv SYN+ACK            | recv SYN
                    | send ACK                | send SYN+ACK
                    |                         v
                    |                +------------------+
                    |                |   SYN_RECEIVED   |
                    |                +------------------+
                    |                         |
                    |                         | recv ACK
                    \                         /
                     \                       /
                      v                     v
                     +-----------------------+
                     |      ESTABLISHED      |
                     +-----------------------+
```

---

## 📝 Descrição dos Estados e Transições

### 1. `TCP_CLOSED` (0)
- **Descrição**: Estado inicial de repouso; o PCB não possui conexão ativa nem passiva.
- **Transição Ativa**: Chamada `connect()` envia um segmento `SYN` e move o PCB para `SYN_SENT`.
- **Transição Passiva**: Chamada `listen()` configura a porta e move o PCB para `LISTEN`.

### 2. `TCP_LISTEN` (1)
- **Descrição**: O PCB escuta conexões de entrada na porta local vinculada.
- **Evento**: Chegada de segmento `SYN` sem flag `ACK` em `tcp_input()`.
- **Ação**: Aloca um novo PCB filho, configura estado `SYN_RECEIVED`, envia segmento `SYN+ACK` ao cliente remoto e inicializa o timer de conexão.

### 3. `TCP_SYN_SENT` (2)
- **Descrição**: Conexão ativa iniciada pelo cliente; aguarda o reconhecimento do servidor.
- **Evento**: Chegada de segmento `SYN+ACK` com `ack_num == pcb->seq_number`.
- **Ação**: Transiciona para `ESTABLISHED`, envia segmento de confirmação `ACK`, arma o timer de Keep-Alive e acorda a tarefa bloqueada em `sys_connect()`.

### 4. `TCP_SYN_RECEIVED` (3)
- **Descrição**: Conexão passiva iniciada pelo servidor; aguarda o `ACK` final do Three-Way Handshake.
- **Evento**: Chegada de segmento `ACK` com `ack_num == pcb->seq_number`.
- **Ação**: Transiciona para `ESTABLISHED`, insere o PCB filho na fila de `accept_head` do listener pai e acorda a thread bloqueada em `sys_accept()`.

### 5. `TCP_ESTABLISHED` (4)
- **Descrição**: Conexão aberta de ponto a ponto bidirecional totalmente estabelecida.
- **Ação**: Permite a transmissão e recepção contínua de segmentos de dados via `socket_vfs_write()` / `socket_vfs_read()`.
