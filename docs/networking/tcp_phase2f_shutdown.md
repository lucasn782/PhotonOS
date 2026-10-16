# TCP Phase 2F — Syscall `shutdown()` e Semântica de Half-Close

## 1. Visão Geral e Conformidade Normativa

A **Phase 2F** introduz na pilha TCP do PhotonOS a syscall `shutdown(int fd, int how)` e a semântica formal de encerramento unidirecional e bidirecional de conexões (*half-close*), em total conformidade com:
- **RFC 793 / RFC 1122**: Encerramento de conexão via envio de FIN mantendo o canal de recepção aberto; enfileiramento sequencial de FIN após esgotamento de dados no buffer de transmissão; processamento de dados do peer após encerramento local de envio.
- **POSIX.1-2017 (`sys/socket.h`)**:
  - `SHUT_RD (0)`: Desabilita futuras operações de recepção no socket. Leituras subsequentes retornam imediatamente EOF (`0`). Dados em trânsito ainda são processados pelo protocolo para manter consistência de ACKs e sequência, mas não entregues à aplicação.
  - `SHUT_WR (1)`: Desabilita futuras operações de envio. Envia um segmento FIN após todo o payload pendente ser transmitido. Permite que a recepção continue operando normalmente (*half-closed local*).
  - `SHUT_RDWR (2)`: Combina `SHUT_RD` e `SHUT_WR`.
  - Rejeição com erro (`-1` / `ENOTCONN` / `EINVAL`) quando invocado sobre sockets não conectados (`CLOSED`, `LISTEN`, `SYN_SENT`, `SYN_RECEIVED`).

---

## 2. Salvaguardas Críticas e Arquitetura

### 2.1. Lifetime do Socket, VFS File Description e PCB
**Problema evitado:** Condição de corrida onde uma thread chama `shutdown()`, enquanto outra chama `close()` ou `socket_vfs_close()`. Se `sys_shutdown` apenas extraísse `sock->tcp` e liberasse o mutex do socket, a thread de `close()` poderia destruir o PCB e o socket, gerando *use-after-free* (UAF).
**Solução adotada:**
- **Proteção da Descrição VFS (`file_description_t`):**
  Introduzida a função `task_get_file_description(task_t *task, int fd)` e `file_description_ref(file_description_t *fdesc)`.
  A consulta ao descritor de arquivo incrementa atomicamente o `fdesc->ref_count` sob desabilitação de interrupções (`cli`/`popfq`).
  Mesmo que um `sys_close()` concorrente remova o fd da tabela de arquivos do processo e execute `file_description_unref()`, o `ref_count` não chega a zero enquanto a syscall em andamento (`sys_shutdown`, `sys_recv`, `sys_send`) não invocar `file_description_unref()`. O fechamento do VFS node e liberação de memória são diferidos com segurança.
- **Hierarquia de Travamento Socket / PCB:**
  Em `sys_shutdown()`:
  1. `mutex_lock(&sock->mutex)`
  2. Valida se `sock->tcp` é não-nulo e se `sock->state == SOCKET_CONNECTED`.
  3. Adquire `spinlock_acquire(&pcb->lock)` **enquanto ainda mantém** `sock->mutex`.
  4. Executa `tcp_shutdown_locked(pcb, how)`.
  5. Libera `spinlock_release(&pcb->lock)`.
  6. Libera `mutex_unlock(&sock->mutex)`.
  Isso elimina qualquer janela de tempo em que `sock->tcp` possa ser desmontado concorrentemente por `socket_vfs_close()`.

### 2.2. Ordenação Atômica entre `send()` e `SHUT_WR`
**Problema evitado:** Reordenação de pacotes ou envio de FIN antes de dados aceitos pela aplicação; inserção de dados por `send()` concorrente após o início do encerramento de escrita.
**Solução adotada:**
- **Ponto de Linearização:** A definição da flag `TCP_PCB_FLAG_SHUT_WR` sob `pcb->lock` estabelece a fronteira estrita.
- Chamadas concorrentes a `tcp_send()` / `sys_send()` verificam `TCP_PCB_FLAG_SHUT_WR` sob `pcb->lock`. Se ativa, rejeitam imediatamente com erro (`-1`), garantindo que nenhum dado seja inserido após a ordem de shutdown.
- **Resolução de Janela Esgotada (`TCP_PCB_FLAG_FIN_PENDING`):**
  Se a aplicação já havia enviado dados que ficaram retidos no buffer `tx_buf.unsent` (por limitação da janela do peer `snd_wnd == 0` ou congestionamento `cwnd`), uma simples chamada a `tcp_drain_unsent()` não consegue transmitir todo o payload.
  Nesse cenário:
  1. O kernel detecta que `pcb->tx_buf.unsent_len > 0`.
  2. A flag `TCP_PCB_FLAG_FIN_PENDING` é ativada no PCB.
  3. O FIN **não** é transmitido prematuramente. O estado permanece em `TCP_ESTABLISHED` (ou `TCP_CLOSE_WAIT`).
  4. Quando o peer reabre a janela anunciada e envia um ACK, `tcp_drain_unsent()` esvazia os dados pendentes na sequência correta.
  5. Assim que `tx_buf.unsent_len == 0`, o FIN é gerado com `seq = pcb->snd_nxt`, e o PCB transiciona para `TCP_FIN_WAIT1` ou `TCP_LAST_ACK`.
  6. Toda a retransmissão e avanço de sequência preservam a contabilidade de FIN e o timer RTO da Phase 2D/2E.

### 2.3. Separação Estrita entre Remote EOF e Local `SHUT_RD`
**Problema evitado:** Confusão de semântica ao reaproveitar `TCP_PCB_FLAG_EOF` para representar `SHUT_RD`.
**Solução adotada:**
- Criada a flag dedicada `TCP_PCB_FLAG_SHUT_RD (1U << 14)`, mantendo `TCP_PCB_FLAG_EOF (1U << 10)` reservada exclusivamente para o recebimento de FIN remoto do peer.
- **Comportamento após `SHUT_RD`:**
  - O buffer de recepção `rx_buf` da conexão é esvaziado imediatamente.
  - Leituras via `sys_recv()` e `socket_vfs_read()` consultam `TCP_PCB_FLAG_SHUT_RD` e retornam `0` (EOF) imediatamente.
  - Se houver leitor bloqueado em espera, `sock->event_cb` é acionado para acordá-lo sem *lost wakeup*.
  - Segmentos subsequentes recebidos com dados válidos em ordem (`seq == rcv_nxt`) são reconhecidos com ACK (mantendo o avanço de `rcv_nxt` consistente no protocolo TCP), mas seu payload é descartado sem ocupar memória de aplicação.
  - Segmentos fora de ordem não avançam `rcv_nxt`.
  - Segmentos combinados `DATA + FIN` continuam sendo processados: o payload é contabilizado e o FIN transiciona a máquina de estados para `CLOSE_WAIT` ou `TIME_WAIT` com ACK correspondente.

### 2.4. Idempotência e Máquina de Estados TCP
**Garantias:**
- Rejeição de sockets nos estados `TCP_CLOSED`, `TCP_LISTEN`, `TCP_SYN_SENT` e `TCP_SYN_RECEIVED`.
- Chamadas consecutivas a `shutdown(SHUT_WR)` ou `shutdown(SHUT_RD)` são idempotentes:
  - Não geram FINs adicionais ou duplicados no fio.
  - Não corrompem buffers nem redefinem ponteiros.
  - Preservam a recepção ativa caso apenas `SHUT_WR` tenha sido solicitado.
  - Preservam os estados `FIN_WAIT1`, `FIN_WAIT2`, `CLOSE_WAIT`, `LAST_ACK` e `TIME_WAIT`.

---

## 3. Matriz de Cobertura de Testes e Resultados

A validação foi realizada em três camadas:
1. **In-Kernel Unit Tests (Boot tests 31 a 42 em `tcp.c`)**:
   - `UNIT_SHUTDOWN_UNCONNECTED`: Rejeição de LISTEN, CLOSED, SYN_SENT, SYN_RECEIVED.
   - `UNIT_SHUTDOWN_SHUT_RD`: Flag dedicada, purga de RX, leitura retorna 0.
   - `UNIT_SHUTDOWN_SHUT_WR`: Transição ESTABLISHED -> FIN_WAIT1 e emissão de FIN.
   - `UNIT_SHUTDOWN_IDEMPOTENCE`: Chamadas repetidas sem FIN duplicado.
   - `UNIT_SHUTDOWN_DATA_ORDER`: Rejeição de `tcp_send()` após `SHUT_WR`.
   - `UNIT_SHUTDOWN_SHUT_RDWR`: Ambas direções desabilitadas atomicamente.
   - `UNIT_SHUTDOWN_CLOSE_COEXISTENCE`: `tcp_close()` após `SHUT_WR` não gera FIN duplicado.
   - `UNIT_SHUTDOWN_CONCURRENT_SEND`: Linearização estrita entre envio e fechamento.
   - `UNIT_SHUTDOWN_WND_EXCEEDED`: Retenção de FIN pendente (`FIN_PENDING`) com janela esgotada.
   - `UNIT_SHUTDOWN_WND_REOPEN`: Dreno sequencial de payload anterior seguido por FIN na reabertura.
   - `UNIT_SHUTDOWN_RD_DATA_FIN`: Processamento consistente de DATA+FIN após `SHUT_RD`.
   - `UNIT_SHUTDOWN_FORK_DUP`: Compartilhamento de fdesc por refcount sem UAF.

2. **Wire Test Harness (`tcptest.c` + `test_tcp_phase2f_shutdown.py`)**:
   - `USER_SHUTDOWN_API_VALIDATION`: Validação da syscall no espaço de usuário.
   - `WIRE_SHUT_WR_HALF_CLOSE`: Host envia dados após guest fechar escrita; guest responde ACK.
   - `WIRE_SHUT_RD_HALF_CLOSE`: Guest fecha leitura; host envia dados que são confirmados e encerra com FIN.
   - `WIRE_SHUT_RDWR_CLOSE`: Fechamento completo bidirecional.
   - `WIRE_ORDER_DRAIN_PAYLOAD`: 1000 bytes transmitidos e recebidos na íntegra antes do FIN do guest.
   - `WIRE_FORK_SHUTDOWN`: Socket herdado por `fork()`, fechado por uma tarefa sem afetar a outra prematuramente.
   - `WIRE_DUP_SHUTDOWN`: Socket duplicado via `dup()`, operado e encerrado ordenadamente.

3. **Análise de Tráfego Wire / PCAP**:
   - `PCAP_FIN_ON_SHUT_WR`: Verificação de flag FIN no fio.
   - `PCAP_DATA_PRECEDES_FIN`: Garantia estrita de que todos os bytes de payload antecedem o FIN.
   - `PCAP_NO_DUPLICATE_FIN`: Nenhuma retransmissão espúria de FIN.
   - `PCAP_CHECKSUMS_VALID`: Checksums TCP/IPv4 100% íntegros.

| Suíte de Testes | Cenários Executados | Resultado |
| :--- | :--- | :--- |
| **TCP Phase 2F Shutdown** | 23 testes (Unit + Wire + Userspace + PCAP) | **23/23 PASS (100%)** |
| **TCP Phase 2E Congestion Control** | 16 testes (Slow start, cwnd, recovery, full duplex) | **16/16 PASS (100%)** |
| **TCP Phase 2D Teardown** | 16 testes (Active/Passive close, simultaneous, dup) | **16/16 PASS (100%)** |
| **TCP Phase 2C Flow Control** | 14 testes (Window dynamics, probe, full duplex) | **14/14 PASS (100%)** |

---

## 4. Comparativo Arquitetural: `shutdown()` vs `close()`

| Aspecto | `shutdown(fd, how)` | `close(fd)` |
| :--- | :--- | :--- |
| **Escopo de Ação** | Conexão TCP no nível de transporte (PCB). | Descritor de arquivo VFS (`file_description_t`). |
| **Efeito em Descritores Compartilhados (`fork`/`dup`)** | Imediato e global para a conexão: `SHUT_WR` encerra o envio TCP para todos os processos que compartilham o socket. | Local ao descritor: o fechamento da conexão só ocorre quando o último descritor VFS aberto for fechado (`ref_count == 0`). |
| **Direcionalidade** | Unidirecional (`SHUT_RD`, `SHUT_WR`) ou bidirecional (`SHUT_RDWR`). | Sempre bidirecional (encerramento do arquivo). |
| **Validade do Descritor** | O descritor permanece aberto e válido no processo (permitindo chamadas de leitura ou escrita na direção não encerrada). | O descritor é liberado da tabela do processo e fica inválido. |
| **Ciclo de Vida do PCB** | Transiciona estados (`ESTABLISHED -> FIN_WAIT1` ou `CLOSE_WAIT -> LAST_ACK`), mantendo o PCB ativo para recepção/ACKs. | Desvincula o socket do PCB e agenda o teardown final da conexão via `tcp_close()`. |

---

## 5. Limitações Remanescentes e Próximos Passos

Embora a máquina de estados de encerramento e a semântica de *half-close* estejam 100% em conformidade com RFC 793, RFC 1122 e POSIX:
1. **Fast Retransmit / Fast Recovery (RFC 5681):** A pilha TCP atual recupera perdas prioritariamente por timeout de retransmissão RTO; o tratamento de 3 ACKs duplicados com Fast Recovery está planejado para o Milestone Phase 2G.
2. **Selective Acknowledgment (SACK - RFC 2018):** Blocos fora de ordem são descartados do avanço de `rcv_nxt` sem opções SACK nos cabeçalhos.
3. **TCP Window Scale (RFC 7323):** Janelas permanecem limitadas ao teto padrão de 16 bits (64 KiB).

