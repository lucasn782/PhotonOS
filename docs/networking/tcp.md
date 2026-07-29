# TCP

O suporte TCP v4.2 foi extraído de `net.c` para `src/kernel/tcp.c`. A versão
atual fornece PCB global, portas, checksum RFC 793, demultiplexação e a base de
sockets de fluxo; não é uma implementação completa da máquina de estados.

Documentação detalhada: [arquitetura](../tcp_architecture.md),
[socket layer](../tcp_socket_layer.md), [PCB](../tcp_pcb.md),
[portas](../tcp_port_management.md), [checksum](../tcp_checksum.md),
[design](../tcp_design.md) e [estados](../tcp_state_machine.md).
