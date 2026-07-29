# 🏗️ Arquitetura do Subsistema TCP — PhotonOS v4.2

## 🌐 Visão Geral

O subsistema TCP (Transmission Control Protocol) do **PhotonOS v4.2** foi projetado como um módulo kernel de rede limpo, modular e desacoplado (`src/kernel/tcp.c`, `include/tcp.h`). Ele estende a pilha IPv4 do sistema operacional sem comprometer a estabilidade do carregador ELF, do gerenciamento de memória (PMM/VMM), da concorrência SMP ou do escalonador preemptivo.

---

## 🔀 Pipeline de Processamento de Datagramas

```text
+-------------------------------------------------------------------+
|                        Driver Intel e1000                         |
|                 (Recepção de Quadros Ethernet DMA)                |
+-------------------------------------------------------------------+
                                  |
                                  v
+-------------------------------------------------------------------+
|                        Camada Ethernet (L2)                       |
|           Validação de EtherType (ETH_TYPE_IPV4 = 0x0800)         |
+-------------------------------------------------------------------+
                                  |
                                  v
+-------------------------------------------------------------------+
|                         Camada IPv4 (L3)                          |
|    Validação de IP local, versão (IPv4) e Checksum do IP Header   |
|            Verificação de Protocolo (IP_PROTO_TCP = 6)            |
+-------------------------------------------------------------------+
                                  |
                                  v
+-------------------------------------------------------------------+
|                          Camada TCP (L4)                          |
|                       `tcp_input()` em tcp.c                      |
|  1. Validação de Checksum TCP RFC 793 (Pseudo Header IPv4 + TCP)  |
|  2. Extração dos Portas e Sequências em Host Byte Order           |
|  3. Demultiplexação via `tcp_lookup()` (Tupla 4-way ou LISTEN)    |
+-------------------------------------------------------------------+
                                  |
                                  v
+-------------------------------------------------------------------+
|                      Protocol Control Block                       |
|                         (`struct tcp_pcb`)                        |
|  1. Atualização do Estado do Protocolo                            |
|  2. Enfileiramento na `receive_queue` (se dados)                  |
|  3. Notificação via `tcp_socket_notify()` (Desperta Threads)      |
+-------------------------------------------------------------------+
                                  |
                                  v
+-------------------------------------------------------------------+
|                      Camada de Sockets & VFS                      |
|                  VFS Node (`socket_vfs_read`)                     |
|    Cópia de Dados para o Buffer de Usuário no Leitor Bloqueado    |
+-------------------------------------------------------------------+
```

---

## 🔬 Componentes Principais

### 1. Núcleo TCP (`src/kernel/tcp.c`)
- **`tcp_init()`**: Inicializa a lista global de PCBs (`tcp_pcbs`) e a mutex global de tabela (`tcp_pcbs_lock`).
- **`tcp_input()`**: Ponto de entrada de segmentos TCP vindos do IPv4.
- **`tcp_output()`**: Monta e envia segmentos TCP via `net_send_ipv4()`.
- **`tcp_checksum()`**: Calcula o checksum do cabeçalho + payload com pseudo-cabeçalho IPv4 (RFC 793).

### 2. Gerenciamento de PCBs (`struct tcp_pcb`)
- Cada PCB representa o estado de uma conexão de transporte local ou socket em escuta.
- Mantido em uma lista encadeada global protegida por `tcp_pcbs_lock`.
- Cada PCB possui sua própria mutex `pcb->lock` para sincronização granular.

### 3. Integração VFS / Sockets (`src/kernel/net.c`)
- Sockets TCP são expostos como arquivos VFS (`VFS_NODE_SOCKET`).
- Suportam chamadas `read()`, `write()`, `bind()`, `connect()`, `listen()` e `accept()`.

---

## 🔒 Concorrência e Sincronização

1. **Lock Hierarchy**:
   - `sockets_mutex` (Camada de Sockets)
   - `tcp_pcbs_lock` (Lista Global de PCBs TCP e Alocação de Portas)
   - `pcb->lock` (Estrutura Individual do PCB)

2. **Prevenção de Deadlocks**:
   - Para aceitar conexões pendentes (`accept`), o listener adquire `listener->lock` antes de manipular a fila `accept_head`.
   - Modificações na lista global exigem a aquisição prévia de `tcp_pcbs_lock`.

---

## 📌 Garantias de Não Regressão
- A camada TCP interage com a rede exclusivamente chamando `net_send_ipv4()`.
- `SOCK_RAW`, `SOCK_DGRAM` (UDP) e `ICMP` permanecem completamente operacionais e inalterados.
