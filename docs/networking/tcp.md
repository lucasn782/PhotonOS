# TCP

O suporte TCP do PhotonOS está dividido em fases progressivas:
* **Fase 1 (Fundação):** PCB global, portas, cálculo e validação de checksum RFC 793 com pseudo-header IPv4, demultiplexação de pacotes e base de sockets de fluxo.
* **Fase 2A (Three-Way Handshake & Conexão Ativa):** Implementação completa do handshake (`SYN -> SYN+ACK -> ACK`), máquina de estados (`CLOSED -> SYN_SENT -> ESTABLISHED`), retransmissão RTO, detecção de RST e timeout, integração com a chamada `connect()` e validação por captura PCAP no fio (*wire*).
* **Fase 2B.1 (Passive Open, Listen, Accept & Backlog):** Implementação completa do lado servidor passivo, chamadas `listen()` e `accept()`, fila de backlog com saturação, demultiplexação prioritária de conexões ativas sobre listeners, ciclo de vida independente de PCBs filhos, bloqueio cooperativo no escalonador e validação de conexões simultâneas/consecutivas com clientes externos via PCAP.
* **Fase 2B.2A (Receive Path & RX Buffer):** Implementação completa do caminho de recepção de dados (`ESTABLISHED`), alocação de ring buffer circular limitado (`tcp_rx_buffer_t`) por conexão, validação de sequência (`rcv_nxt`), descarte de dados duplicados e fora de ordem, cálculo da janela anunciada, chamada de sistema `recv()`, bloqueio e despertar cooperativo no escalonador, leituras parciais, EOF (`TCP_CLOSE_WAIT`) e RST.
* **Fase 2B.2B (Transmit Path & `send()`):** Caminho de transmissão por PCB com `tcp_tx_buffer_t` limitado a 8192 bytes, cópia segura de payload Ring 3, segmentação em MSS 1460, `SND.NXT`/`SND.UNA`, ACK parcial/cumulativo, RTO básico com limite de tentativas e limpeza em RST/close. A API usa escrita parcial, não bloqueante, quando o TX buffer enche.

Para as especificações técnicas detalhadas e evidências experimentais:
* [TCP Phase 2A — Arquitetura e Validação](tcp_phase2a.md)
* [TCP Phase 2B.1 — Passive Open, Listen, Accept & Backlog](tcp_phase2b_passive.md)
* [TCP Phase 2B.2A — Receive Path, RX Buffer & recv()](tcp_phase2b2_rx.md)
* [TCP Phase 2B.2B — TX Path, send() & Data ACK](tcp_phase2b2_tx.md)

Documentação técnica de referência:
* [Arquitetura TCP](../tcp_architecture.md)
* [Camada de Sockets TCP](../tcp_socket_layer.md)
* [Protocol Control Block (PCB)](../tcp_pcb.md)
* [Gerenciamento de Portas](../tcp_port_management.md)
* [Checksum RFC 793](../tcp_checksum.md)
* [Design e Modularidade](../tcp_design.md)
* [Máquina de Estados](../tcp_state_machine.md)
