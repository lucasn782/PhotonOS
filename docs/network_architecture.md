# Arquitetura de Rede e Sockets POSIX (v1.7)

O PhotonOS (v1.7) implementa uma pilha de rede integrada ao Virtual File System (VFS), fornecendo chamadas de sistema baseadas em POSIX para a criação, vinculação e leitura de sockets em espaço de usuário (Ring 3).

---

## 1. Abstração de Sockets no VFS

A interface de sockets é unificada com a abstração de arquivos através do VFS. As seguintes chamadas de sistema foram disponibilizadas para Ring 3:

*   **`sys_socket` (Syscall 24)**: Aloca uma estrutura interna `struct socket` no kernel e cria um nó de arquivo anônimo (`vfs_node_t`) do tipo `VFS_NODE_DEVICE`. Retorna um File Descriptor (FD) único para o processo chamador.
*   **`sys_bind` (Syscall 25)**: Vincula um endereço local e uma porta específica à estrutura do socket correspondente.

### Ciclo de Vida do VFS Node
Quando um processo chama `sys_close` (Syscall 15), o Kernel invoca automaticamente o callback de liberação `node->close(node)`. Para sockets, isso executa a rotina `socket_vfs_close`, que:
1. Desativa o socket de forma segura.
2. Esvazia a fila de pacotes pendentes e libera a memória de todos os payloads usando `kfree()`.
3. Libera o nó de VFS alocado no Kernel Heap.

---

## 2. Proteção Atômica dos Ring Buffers

Em um ambiente multitarefa preemptivo, a inserção de pacotes pelo driver `e1000` (no contexto de IRQ/Thread de rede) e a remoção dos pacotes pelo processo de usuário (via `read()`) podem causar condições de corrida críticas nos ponteiros circulares de leitura (`rx_head`/`rx_tail`).

Para mitigar isso, as rotinas críticas de enfileiramento e desenfileiramento de pacotes utilizam o isolamento atômico de interrupções locais:

```c
static inline uint64_t save_and_disable_interrupts(void)
{
    uint64_t rflags;
    __asm__ volatile ("pushfq; popq %0" : "=r"(rflags));
    __asm__ volatile ("cli" ::: "memory");
    return rflags;
}

static inline void restore_interrupts(uint64_t rflags)
{
    if ((rflags & 0x200ULL) != 0) {
        __asm__ volatile ("sti" ::: "memory");
    }
}
```

Isso garante que a preempção pelo temporizador PIT (INT 0x20) seja temporariamente desativada, impedindo que o Kernel seja interrompido no meio da manipulação de ponteiros de head, tail e contadores do buffer circular.

---

## 3. Validação Matemática de Checksum (RFC 768)

Para garantir a robustez na integridade dos dados trafegados, todas as camadas de parse aplicam validações estritas de checksum. Qualquer pacote inválido é descartado silenciosamente:

### Camada IP e ICMP
As somas de verificação interpretam o fluxo de bytes interpretando palavras de 16 bits em formato big-endian.
*   **IP Header Checksum**: O resultado de `net_checksum(ip, ip_header_length)` deve ser exatamente `0`.
*   **ICMP Checksum**: O resultado de `net_checksum(icmp, payload_len)` deve ser exatamente `0`.

### Camada UDP (RFC 768)
A validação do checksum UDP requer o cálculo incluindo o **pseudo-cabeçalho** IPv4:
*   IP de Origem (4 bytes)
*   IP de Destino (4 bytes)
*   Protocolo (1 byte)
*   Tamanho UDP (2 bytes)

Se o checksum calculado não bater com o cabeçalho recebido, o pacote é imediatamente liberado sem ser encaminhado aos sockets de usuário.

---

## 4. I/O Não-Bloqueante e Timeout (`-EAGAIN`)

A leitura de sockets via `sys_read` opera no modo não-bloqueante seguro para evitar congelamentos do Shell ou de utilitários caso a conexão caia.

Se um processo chamar `read()` em um socket cujo ring buffer estiver vazio (`rx_count == 0`), a função retorna imediatamente o código `-11` (`-EAGAIN`), permitindo que a aplicação faça polling e ceda tempo de CPU de forma controlada (`yield()`).

---

## 5. Regras de Design de Logs do Kernel

Em total conformidade com a política de hardening do PhotonOS, nenhuma chamada de depuração ou log de descarte/erros na camada de rede utiliza especificadores de formato (`%d`, `%x`, `%s`). Todos os logs em Ring 0 são estritamente estáticos:

```c
klog("NET: Checksum IP invalido.\n");
klog("NET: Checksum UDP invalido.\n");
klog("NET: Buffer do socket cheio, descartando pacote.\n");
klog("NET: Pacote descartado por falta de socket ativo.\n");
```
