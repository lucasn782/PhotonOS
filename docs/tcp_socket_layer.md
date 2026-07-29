# 🔌 Camada de Sockets & VFS TCP — PhotonOS v4.2

## 🌐 Visão Geral

A camada de Sockets do **PhotonOS v4.2** fornece a interface de abstração POSIX para aplicações em Ring 3 utilizarem a pilha TCP/IP. Ela conecta descritores de arquivos VFS (`vfs_node_t`) às estruturas internas de controle do TCP (`struct tcp_pcb`) e integra-se diretamente ao escalonador preemptivo.

---

## 🛠️ Abstração de Sockets VFS

Ao criar um socket `socket(AF_INET, SOCK_STREAM, 0)`, o kernel realiza as seguintes operações:
1. Aloca um slot na tabela de sockets (`sockets[MAX_SOCKETS]`).
2. Instancia um novo PCB TCP via `tcp_socket_create(sock)`.
3. Aloca um nó VFS do tipo `VFS_NODE_SOCKET` com os ponteiros de função:
   - `read` -> `socket_vfs_read()`
   - `write` -> `socket_vfs_write()`
   - `close` -> `socket_vfs_close()`
4. Associa o nó VFS a um File Descriptor (`fd`) na tarefa atual (`task_alloc_fd()`).

---

## 🔄 Fluxo das Syscalls de Socket TCP

### 1. `sys_socket(domain, type, protocol)`
- Valida os parâmetros (`AF_INET`, `SOCK_STREAM`, `IP_PROTO_TCP`).
- Cria a associação bidirecional entre `struct socket` e `struct tcp_pcb`.

### 2. `sys_bind(fd, addr, addrlen)`
- Valida o ponteiro de memória do usuário via VMM.
- Invoca `tcp_bind(pcb, local_ip, local_port)`.
- Garante a associação da porta e endereço local.

### 3. `sys_connect(fd, addr, addrlen)`
- Transiciona o estado do PCB para `TCP_SYN_SENT`.
- Envia o segmento de inicialização `SYN` via `tcp_output()`.
- Coloca a thread em estado de espera (`TASK_WAIT_NETWORK`) sem espera ocupada ("busy wait").
- Acordada quando o segmento `SYN+ACK` for recebido e o estado mudar para `ESTABLISHED`.

### 4. `sys_listen(fd, backlog)`
- Transiciona o estado do PCB para `TCP_LISTEN`.
- Configura o limite de conexões pendentes (`backlog`, limitado a `TCP_MAX_BACKLOG = 8`).

### 5. `sys_accept(fd, addr, addrlen)`
- Tenta retirar um filho da fila `accept_head` do listener via `tcp_accept()`.
- Se a fila estiver vazia, coloca a thread para adormecer em `TASK_WAIT_SOCKET_RECV`.
- Quando um novo cliente atinge `ESTABLISHED`, a thread é acordada, um novo File Descriptor é alocado e retornado ao espaço de usuário com os dados do cliente remoto (`sockaddr_in`).

### 6. `socket_vfs_read()` & `socket_vfs_write()`
- **Escrita (`write`)**: Transmite dados formatados no segmento TCP via `tcp_output()`.
- **Leitura (`read`)**: Consome dados ordenados da `receive_queue` do PCB. Caso esteja vazia e o PCB não esteja no estado `TCP_CLOSED`, a tarefa adormece em `TASK_WAIT_SOCKET_RECV`. Se o PCB for fechado, retorna `0` (EOF).

---

## 🔒 Integração com o Escalonador & Despertar Sem Carga Útil (Zero Busy-Wait)

Quando um segmento TCP é recebido pela interrupção do e1000 e processado por `tcp_input()`:
1. Os dados são salvos na `receive_queue` do PCB.
2. O subsistema invoca `tcp_socket_notify(socket)`.
3. `tcp_socket_notify()` executa `scheduler_wake_socket(socket)` sob desativação de interrupções (`cli`), retirando a tarefa do estado de espera e colocando-a na fila de prontas (`TASK_STATE_READY`).
