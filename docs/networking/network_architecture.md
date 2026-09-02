# Arquitetura de Rede, Barramento PCI e Sockets POSIX (v2.0)

O PhotonOS v2.0 implementa uma pilha de rede totalmente integrada com o barramento PCI, inicializando controladores de rede via barramento de DMA físico puro e disponibilizando sockets integrados ao Virtual File System (VFS) com suporte a chamadas POSIX no espaço de usuário (Ring 3).

---

## 1. Varredura e Sondagem de Hardware (PCI Bus Scan)

A ativação do subsistema de rede inicia-se com a varredura dinâmica do barramento PCI pelo Kernel. Esta rotina é responsável por localizar o hardware do controlador Ethernet (Intel e1000) e configurar seus canais de E/S.

*   **Mecanismo de Acesso:** A comunicação com o espaço de configuração do barramento PCI é realizada via portas de E/S padrão da arquitetura x86_64:
    *   `0xCF8` (PCI Config Address): Armazena o endereço do dispositivo (Bus, Slot, Function e Register Offset) que se deseja ler ou escrever.
    *   `0xCFC` (PCI Config Data): Canal de transferência para leitura e escrita dos dados de configuração de 32 bits correspondentes.
*   **Algoritmo de Sondagem:**
    1.  O Kernel realiza uma varredura completa varrendo todos os 256 barramentos possíveis (`bus`), 32 slots por barramento (`slot`) e 8 funções por slot (`func`).
    2.  O registro de offset `0x08` (PCI Class Code Offset) é lido para cada dispositivo ativo.
    3.  A filtragem é baseada no Class Code `0x02` (Network Controller) e Subclass Code `0x00` (Ethernet Controller), garantindo o acoplamento do driver Intel e1000 apenas aos dispositivos de rede compatíveis.
    4.  Uma vez localizado, os registradores BAR0/BAR1 do dispositivo são lidos para identificar o endereço base do mapeamento de E/S em memória (MMIO BAR).

---

## 2. DMA Físico Puro e Bus Mastering

Para contornar o problema crônico de perda de pacotes em taxas de transferência elevadas, o driver e1000 foi homologado para utilizar um pipeline de DMA físico puro coordenado com o Physical Memory Manager (PMM).

*   **Alocação de Descritores RX/TX:** Os anéis de descritores de transmissão (TX Ring) e recepção (RX Ring) são alocados diretamente em frames de memória física de 4KB usando o PMM (`pmm_alloc`).
*   **Mapeamento de Buffers de Pacotes:** Os buffers de payloads individuais associados a cada descritor das filas RX e TX são alocados através de chamadas consecutivas a `pmm_alloc()`. O driver converte esses endereços virtuais em seus correspondentes físicos reais utilizando o VMM (`vmm_virt_to_phys`), escrevendo os endereços físicos diretamente na estrutura de controle do hardware (`desc->addr`).
*   **Bus Mastering:** O driver PCI configura o registrador de Comando do dispositivo (`PCI_COMMAND_OFFSET` = `0x04`) ativando a flag de *Bus Master* (`PCI_COMMAND_BUS_MASTER` = `1U << 2`), permitindo que a placa de rede efetue transferências DMA bidirecionais diretamente com a memória RAM física sem passar pelo gargalo da CPU.
*   **Mitigação de Paging Faults:** Ao atrelar o DMA a frames físicos reais e contíguos obtidos via PMM e mapeados de forma estática, elimina-se o risco de falhas de página durante o recebimento assíncrono de pacotes. Isso viabiliza o funcionamento pleno e nativo do comando `ping` no Shell.

---

## 3. Abstração de Sockets no VFS

A interface de sockets é unificada com a abstração de arquivos através do VFS. As seguintes chamadas de sistema foram disponibilizadas para Ring 3:

*   **`sys_socket` (Syscall 24)**: Aloca uma estrutura interna `struct socket` no kernel e cria um nó de arquivo anônimo (`vfs_node_t`) do tipo `VFS_NODE_DEVICE`. Retorna um File Descriptor (FD) único para o processo chamador.
*   **`sys_bind` (Syscall 25)**: Vincula um endereço local e uma porta específica à estrutura do socket correspondente.

### Ciclo de Vida do VFS Node
Quando um processo chama `sys_close` (Syscall 15), o Kernel invoca automaticamente o callback de liberação `node->close(node)`. Para sockets, isso executa a rotina `socket_vfs_close`, que:
1.  Desativa o socket de forma segura.
2.  Esvazia a fila de pacotes pendentes e libera a memória de todos os payloads usando `kfree()`.
3.  Libera o nó de VFS alocado no Kernel Heap.

---

## 4. Proteção Atômica dos Ring Buffers

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

## 5. Validação Matemática de Checksum (RFC 768)

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

## 6. I/O Não-Bloqueante e Timeout (`-EAGAIN`)

A leitura de sockets via `sys_read` opera no modo não-bloqueante seguro para evitar congelamentos do Shell ou de utilitários caso a conexão caia.

Se um processo chamar `read()` em um socket cujo ring buffer estiver vazio (`rx_count == 0`), a função retorna imediatamente o código `-11` (`-EAGAIN`), permitindo que a aplicação faça polling e ceda tempo de CPU de forma controlada (`yield()`).

---

## 7. Regras de Design de Logs do Kernel

Em total conformidade com a política de hardening do PhotonOS, nenhuma chamada de depuração ou log de descarte/erros na camada de rede utiliza especificadores de formato (`%d`, `%x`, `%s`). Todos os logs em Ring 0 são estritamente estáticos:

```c
klog("NET: Checksum IP invalido.\n");
klog("NET: Checksum UDP invalido.\n");
klog("NET: Buffer do socket cheio, descartando pacote.\n");
klog("NET: Pacote descartado por falta de socket ativo.\n");
```

---

## 8. Subsistema TCP (Fase 1 — v4.2)

O PhotonOS implementa a infraestrutura base do protocolo TCP (RFC 793 / RFC 1071) totalmente desacoplada e integrada à camada IPv4 e Socket Layer:

*   **Protocol Control Block (`tcp_pcb_t`)**: Gerenciador de conexões dinâmico mantido em lista global (`tcp_pcbs`) protegida por `tcp_pcbs_lock` e per-PCB `lock`.
*   **Máquina de Estados RFC 793**: Definição completa dos 10 estados (`CLOSED`, `LISTEN`, `SYN_SENT`, `SYN_RECEIVED`, `ESTABLISHED`, `FIN_WAIT1`, `FIN_WAIT2`, `CLOSE_WAIT`, `LAST_ACK`, `TIME_WAIT`).
*   **Checksum TCP RFC 793 / 1071**: Soma de verificação com pseudo-cabeçalho IPv4 (src_ip, dest_ip, proto=6, tcp_length) em palavras de 16-bits.
*   **Parser e Serializador**: Funções reusáveis `tcp_parse_header` e `tcp_serialize_header` com validação de offset e flags.
*   **Integração Socket Stream**: Suporte a `socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)` com alocação automática de PCB, gerenciamento de descritores no contexto do kernel e suporte transparente em Ring 3.

