# 🎯 Princípios de Design do Subsistema TCP — PhotonOS v4.2

## 🌐 Visão Geral

O design do subsistema TCP no **PhotonOS v4.2** foi guiado por princípios rígidos de engenharia de software de sistemas operacionais: **modularidade**, **extensibilidade**, **isolamento de subsistemas** e **ausência total de espera ocupada ("zero busy-wait")**.

---

## 🏛️ Princípios Arquiteturais

### 1. Desacoplamento Estrito
- O módulo TCP (`src/kernel/tcp.c`, `include/tcp.h`) não possui dependências diretas de drivers de hardware específicos (como o `e1000.c`).
- O envio de datagramas IPv4 é feito exclusivamente através da API abstrata `net_send_ipv4()`.
- O descarte e roteamento de quadros Ethernet/IP ocorre na camada IP (`net.c`), mantendo `tcp.c` focado unicamente na lógica de transporte e gerenciamento de PCBs.

### 2. Ausência de Espera Ocupada (Zero Busy-Wait)
- Nenhuma função do subsistema TCP utiliza laços de repetição vazios (`while(1)`) para aguardar eventos de rede.
- As chamadas de sistema blocking (`connect`, `accept`, `read`) utilizam primitiva `scheduler_sleep_current()` especificando estados de espera explícitos (`TASK_WAIT_NETWORK`, `TASK_WAIT_SOCKET_RECV`).
- O tratamento de interrupções e a thread de background de rede acordam unicamente as tarefas diretamente interessadas via `scheduler_wake_socket()`.

### 3. Isolamento da Memória e Segurança
- Todas as estruturas globais do módulo TCP (`tcp_pcbs`, `tcp_pcbs_lock`, `tcp_next_ephemeral`) são alocadas na seção dedicada `.network_state`.
- Isso previne que variáveis de estado de rede sejam corrompidas quando a área `.bss` cruza a abertura legada de memória de vídeo VGA.

---

## 🗺️ Roadmap de Evolução (Fase 1 vs Fase 2)

| Recurso | Fase 1 (v4.2 — Atual) | Fase 2 (Futura) |
|---|---|---|
| **Estados Suportados** | `CLOSED`, `LISTEN`, `SYN_SENT`, `SYN_RECEIVED`, `ESTABLISHED` | `FIN_WAIT_1`, `FIN_WAIT_2`, `CLOSE_WAIT`, `CLOSING`, `LAST_ACK`, `TIME_WAIT` |
| **Handshake** | Three-Way Handshake Básico | Opções TCP (MSS, Window Scale, SACK) |
| **Temporizadores** | Infraestrutura com `struct tcp_timers` armada | Retransmissão automática por estouro de RTO e Keep-Alive Ativo |
| **Controle de Fluxo** | Janela Fixa (`TCP_DEFAULT_WINDOW = 65535`) | Janela Deslizante Dinâmica e Controle de Congestionamento (Slow Start) |
| **Servidor Web** | Suporte a Sockets e Ring 3 | Servidor HTTP nativo em Ring 3 no VFS/FAT16 |
