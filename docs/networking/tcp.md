# TCP

O suporte TCP do PhotonOS está dividido em fases progressivas:
* **Fase 1 (Fundação):** PCB global, portas, cálculo e validação de checksum RFC 793 com pseudo-header IPv4, demultiplexação de pacotes e base de sockets de fluxo.
* **Fase 2A (Three-Way Handshake & Conexão Ativa):** Implementação completa do handshake (`SYN -> SYN+ACK -> ACK`), máquina de estados (`CLOSED -> SYN_SENT -> ESTABLISHED`), retransmissão RTO, detecção de RST e timeout, integração com a chamada `connect()` e validação por captura PCAP no fio (*wire*).

Para a especificação técnica detalhada e evidências experimentais da Fase 2A, consulte:
* [TCP Phase 2A — Arquitetura e Validação](tcp_phase2a.md)

Documentação técnica de referência:
* [Arquitetura TCP](../tcp_architecture.md)
* [Camada de Sockets TCP](../tcp_socket_layer.md)
* [Protocol Control Block (PCB)](../tcp_pcb.md)
* [Gerenciamento de Portas](../tcp_port_management.md)
* [Checksum RFC 793](../tcp_checksum.md)
* [Design e Modularidade](../tcp_design.md)
* [Máquina de Estados](../tcp_state_machine.md)
