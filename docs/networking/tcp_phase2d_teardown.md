# 🛑 TCP Phase 2D — Encerramento Gracioso de Conexões e Teardown (RFC 793)

## 1. Visão Geral

O marco **TCP Phase 2D** implementa o ciclo de vida completo de encerramento gracioso (*graceful connection teardown*) da pilha TCP do **PhotonOS**, em total conformidade com a especificação RFC 793.

Com esta fase, a pilha TCP atinge maturidade operacional completa: estabelece conexões ativas e passivas (handshake de 3 vias), gerencia fluxo bidirecional de dados com controle de fluxo por janela deslizante e persist timer, e realiza o desligamento ordenado em ambas as direções garantindo entrega total de dados em trânsito, semântica correta de EOF (retorno 0 em `recv()`), retransmissão de controle em caso de perda de pacotes, isolamento de TIME_WAIT e liberação segura de recursos (*memory safety* e *refcounting*).

---

## 2. Máquina de Estados de Fechamento (RFC 793)

A máquina de estados TCP foi estendida para suportar todos os estados de encerramento definidos pela especificação:

```text
               +---------+
               |  CLOSED |
               +---------+
                    |
                    | (active open / passive open)
                    v
             +---------------+
             |  ESTABLISHED  |
             +---------------+
                /         \
   local close /           \ remote FIN
              v             v
       +------------+ +------------+
       | FIN_WAIT_1 | | CLOSE_WAIT |
       +------------+ +------------+
        /    |   \           |
 remote/ peer| peer\    local| close
    FIN/  FIN|  ACK \        |
       v  +ACKv      v       v
   +-------+  +------------+ +----------+
   |CLOSING|  | FIN_WAIT_2 | | LAST_ACK |
   +-------+  +------------+ +----------+
       |             |             |
   peer| ACK   remote| FIN     peer| ACK
       v             v             v
      +---------------+       +--------+
      |   TIME_WAIT   |       | CLOSED |
      +---------------+       +--------+
             |
   timeout   | (200 ticks = 2.0s)
             v
         +--------+
         | CLOSED |
         +--------+
```

### 2.1 Tabela de Transições da Máquina de Estados

| Estado Atual | Evento | Próximo Estado | Segmento Transmitido | Ação Arquitetural |
|---|---|---|---|---|
| **ESTABLISHED** | Chamada `close()` local (sem dados pendentes) | **FIN_WAIT_1** | FIN + ACK (`seq=SND.NXT`) | Avança `SND.NXT++`, arma temporizador RTO para FIN, desvincula socket (`pcb->socket = 0`). |
| **ESTABLISHED** | Chamada `close()` local (com dados em `unsent`) | **FIN_WAIT_1** | Dados (`unsent`) seguidos de FIN + ACK | Executa `tcp_drain_unsent()`, transmite dados ordenados e em seguida FIN com `seq=SND.NXT`. |
| **ESTABLISHED** | Segmento com `FIN` recebido do par | **CLOSE_WAIT** | ACK (`ack=RCV.NXT+1`) | Incrementa `RCV.NXT++`, ativa flag `TCP_PCB_FLAG_EOF`, acorda tarefas em `sys_recv()`. |
| **FIN_WAIT_1** | ACK do nosso FIN recebido (`ACK >= SND.NXT`) | **FIN_WAIT_2** | Nenhum | Desarma temporizador RTO do FIN, aguarda FIN do par. |
| **FIN_WAIT_1** | Segmento com `FIN` do par (sem ACK do nosso FIN) | **FIN_WAIT_1** (Closing) | ACK (`ack=RCV.NXT+1`) | Incrementa `RCV.NXT++`, marca `TCP_PCB_FLAG_EOF`, aguarda ACK do nosso FIN (fechamento simultâneo). |
| **FIN_WAIT_1** | Segmento com `FIN` + `ACK` do nosso FIN combinado | **TIME_WAIT** | ACK (`ack=RCV.NXT+1`) | Incrementa `RCV.NXT++`, desarma RTO, arma temporizador TIME_WAIT (200 ticks). |
| **FIN_WAIT_1** | Expiração de RTO (perda do ACK do FIN) | **FIN_WAIT_1** | FIN + ACK (`seq=SND.NXT-1`) | Retransmite FIN com backoff exponencial. Se atingir `TCP_MAX_DATA_RETRIES`, move para CLOSED. |
| **FIN_WAIT_2** | Segmento com `FIN` recebido do par | **TIME_WAIT** | ACK (`ack=RCV.NXT+1`) | Incrementa `RCV.NXT++`, marca `TCP_PCB_FLAG_EOF`, arma temporizador TIME_WAIT (200 ticks). |
| **FIN_WAIT_2** | Segmento com dados em ordem recebido | **FIN_WAIT_2** | ACK (`ack=RCV.NXT`) | Armazena dados no buffer RX, avança `RCV.NXT` e confirma recepção. |
| **CLOSE_WAIT** | Chamada `recv()` da aplicação com dados | **CLOSE_WAIT** | Nenhum | Entrega dados do buffer RX (`read_bytes > 0`). |
| **CLOSE_WAIT** | Chamada `recv()` com buffer RX vazio | **CLOSE_WAIT** | Nenhum | Retorna `0` (EOF POSIX imediato sem bloqueio). |
| **CLOSE_WAIT** | FIN duplicado recebido do par | **CLOSE_WAIT** | ACK (`ack=RCV.NXT`) | Re-emite ACK sem alterar `RCV.NXT` nem corromper estado. |
| **CLOSE_WAIT** | Chamada `close()` local da aplicação | **LAST_ACK** | FIN + ACK (`seq=SND.NXT`) | Drena eventuais dados pendentes, avança `SND.NXT++`, arma RTO. |
| **LAST_ACK** | ACK do nosso FIN recebido (`ACK >= SND.NXT`) | **CLOSED** | Nenhum | Desarma timers, desregistra PCB e desaloca memória se `socket == 0`. |
| **LAST_ACK** | FIN duplicado recebido do par | **LAST_ACK** | ACK (`ack=RCV.NXT`) | Re-emite ACK sem avançar sequência. |
| **LAST_ACK** | Expiração de RTO do FIN | **LAST_ACK** | FIN + ACK (`seq=SND.NXT-1`) | Retransmite FIN com backoff exponencial. |
| **TIME_WAIT** | FIN duplicado recebido do par | **TIME_WAIT** | ACK (`ack=RCV.NXT`) | Re-emite ACK e reinicia temporizador `TIME_WAIT = now + 200 ticks`. |
| **TIME_WAIT** | Expiração do temporizador (200 ticks decorridos) | **CLOSED** | Nenhum | Move para CLOSED, desregistra PCB globalmente e desaloca memória. |

---

## 3. Contabilidade de Sequência do FIN (FIN Accounting)

Conforme a RFC 793, o flag de controle `FIN` consome exatamente **1 número de sequência** no espaço de endereçamento da conexão:

$$\text{FIN\_SEQ} = \text{SND.NXT}_{\text{anterior}}$$
$$\text{SND.NXT}_{\text{novo}} = \text{FIN\_SEQ} + 1$$

### 3.1 Transmissão de FIN
Ao despachar o segmento com `TCP_FLAG_FIN`:
1. `header->seq_num = htonl(pcb->snd_nxt)`
2. `pcb->snd_nxt++`
3. `pcb->seq_number = pcb->snd_nxt`

### 3.2 Reconhecimento pelo Par (ACK)
O receptor deve obrigatoriamente acusar a recepção do FIN enviando um ACK com:
$$\text{ACK\_NUM} = \text{FIN\_SEQ} + 1 = \text{SND.NXT}$$

No lado que enviou o FIN, a condição de confirmação válida é verificada por aritmética de números de sequência (RFC 1982):
$$\text{TCP\_SEQ\_GE}(\text{acknowledgement}, \text{pcb->snd\_nxt})$$

Quando essa condição é satisfeita, `fin_acked = 1`, o temporizador de retransmissão RTO é cancelado e a transição de estado ocorre com precisão matemática.

---

## 4. Active Close (Fechamento Ativo)

Quando a aplicação local inicia o encerramento invocando `close(fd)`:
1. **Mapeamento de Descritor e Refcounting:**
   $$\text{fd} \longrightarrow \text{file\_description\_t} \longrightarrow \text{vfs\_node\_t} \longrightarrow \text{struct socket} \longrightarrow \text{struct tcp\_pcb}$$
   A função `file_description_unref(fdesc)` decrementa a contagem de referências do descritor aberto. Se outros descritores ou processos ainda referenciarem o mesmo socket (via `dup()` ou `fork()`), `node->close()` **não** é chamado, preservando a conexão viva.
2. **Desconexão do Socket:** Quando `ref_count == 0`, `socket_vfs_close()` é acionado:
   - Marca `sock->active = 0`.
   - Limpa `sock->tcp = 0`.
   - Invoca `tcp_close(pcb)`.
3. **Drenagem de Dados Pendentes:** Em `tcp_close(pcb)`, se a conexão estiver em `ESTABLISHED`, quaisquer dados presentes na fila `pcb->tx_buf.unsent` são transmitidos através de `tcp_drain_unsent(pcb)`.
4. **Emissão de FIN:** Imediatamente após os dados, o kernel sintetiza um segmento `FIN + ACK` com número de sequência consecutivo aos dados transmitidos.
5. **Transição para `FIN_WAIT_1`:** O estado muda para `TCP_FIN_WAIT1`, o temporizador RTO é armado (`TCP_PCB_FLAG_TIMER_RTO`) e o segmento é entregue ao hardware com locks da pilha TCP liberados.
6. **Desacoplamento de Ciclo de Vida:** O ponteiro `pcb->socket` é zerado (`pcb->socket = 0`). O PCB permanece registrado na lista global `tcp_pcbs` para continuar recebendo os pacotes de fechamento do par no background.

---

## 5. Passive Close e Semântica de EOF (Fechamento Passivo)

Quando o peer remoto fecha a conexão primeiro:
1. **Recepção do FIN:** Em `tcp_input()`, o segmento com flag `FIN` é recebido:
   - Se houver payload conjunto (DATA + FIN), o payload é entregue primeiro ao buffer circular de recepção (`tcp_rx_buffer_t`) e `rcv_nxt` avança pelo comprimento dos dados.
   - O FIN é processado: `pcb->rcv_nxt += 1U`.
   - O flag de fim de fluxo é marcado: `pcb->flags |= TCP_PCB_FLAG_EOF`.
   - O estado transiciona para `TCP_CLOSE_WAIT`.
   - Um segmento `ACK` confirmando o FIN do par é emitido imediatamente no fio.
   - As tarefas suspensas aguardando dados em `sys_recv()` são acordadas (`tcp_socket_notify`).
2. **Consumo de Dados Restantes em `sys_recv()`:**
   - Enquanto `pcb->rx_buf.used > 0`, chamadas subsequentes a `recv()` entregam normalmente os dados pendentes que chegaram antes ou junto com o FIN.
   - Somente quando `pcb->rx_buf.used == 0` **E** `(pcb->flags & TCP_PCB_FLAG_EOF) != 0` (ou estado `TCP_CLOSE_WAIT`), `sys_recv()` retorna **0** (EOF POSIX).
   - Múltiplas chamadas sucessivas a `recv()` após o EOF continuam retornando 0 imediatamente sem bloqueio nem ressuscitação indevida.
3. **Fechamento pelo Processo Local:**
   - A aplicação detecta o EOF (0) e chama `close(fd)`.
   - `tcp_close(pcb)` detecta `state == TCP_CLOSE_WAIT`.
   - Emite o segmento `FIN + ACK` local, transiciona para `TCP_LAST_ACK` e arma o temporizador RTO.
   - Ao receber o `ACK` do peer confirmando nosso FIN, o estado muda para `TCP_CLOSED`, o PCB é desregistrado e a memória é liberada com segurança.

---

## 6. TIME_WAIT e Temporizador Seguro

### 6.1 Intervalo Escolhido e Justificativa
No PhotonOS, o estado `TCP_TIME_WAIT` utiliza um intervalo de **200 ticks**:
$$\text{Duração} = 200 \times 10\text{ ms} = 2{,}0\text{ segundos}$$

*Justificativa:* No padrão RFC 793, o intervalo teórico é $2 \times \text{MSL}$ (tipicamente 120 segundos). Em um sistema operacional experimental freestanding executado em ambiente QEMU com suítes de testes automatizados, esperar 120 segundos travaria a execução de CI/CD. O intervalo de 2.0 segundos (200 ticks do PIT/APIC a 100 Hz) é longo o suficiente para absorver retransmissões de pacotes perdidos e FINs duplicados no tráfego local/wire, mas determinístico e ágil para reutilização segura de portas.

### 6.2 Prevenção de Use-After-Free (UAF)
Durante o estado `TIME_WAIT`:
- O PCB **não** é destruído imediatamente. Ele continua ativo na tabela `tcp_pcbs`.
- Se o peer remoto não tiver recebido o último ACK e retransmitir seu FIN, o PCB em `TIME_WAIT` intercepta o pacote, re-emite o ACK e reinicia o temporizador (`timeout = now + 200 ticks`).
- Quando o tick do temporizador atinge `now_ticks >= pcb->timers.timeout`:
  1. O estado muda para `TCP_CLOSED`.
  2. Os temporizadores são zerados (`tcp_timers_reset`).
  3. O PCB é adicionado ao vetor local `dead_pcbs`.
  4. O lock global `tcp_pcbs_lock` é liberado.
  5. A rotina `tcp_free(pcb)` é chamada para cada PCB morto fora de qualquer lock, desregistrando-o da lista global e liberando a memória do heap.

---

## 7. Retransmissão de FIN

Se o segmento FIN enviado não receber o respectivo ACK antes do tempo limite de retransmissão:
1. O temporizador RTO (`pcb->timers.retransmission`) expira em `tcp_timer_tick()`.
2. A política de retransmissão avalia as filas:
   - Se houver dados pendentes não confirmados em `unacked`, o segmento mais antigo de dados é retransmitido com prioridade.
   - Se não houver dados pendentes em `unacked`, o segmento `FIN + ACK` é retransmitido com `seq = SND.NXT - 1`.
3. O intervalo RTO sofre *backoff exponencial* (`rto_ticks * 2`), limitado a 1000 ticks.
4. O contador `retransmit_count` é incrementado. Ao atingir `TCP_MAX_DATA_RETRIES` (5 tentativas), a conexão é abortada para `TCP_CLOSED` para evitar vazamento de memória.
5. A transmissão diferida (`tcp_deferred_tx`) envia o pacote sem travar locks do subsistema.

---

## 8. Fechamento Simultâneo (Simultaneous Close)

No cenário em que ambos os lados da conexão iniciam o fechamento concorrentemente:
1. Ambos os lados enviam FIN e entram em `FIN_WAIT_1`.
2. O endpoint recebe o FIN do par enquanto ainda aguarda o ACK de seu próprio FIN.
3. O kernel reconhece o FIN do par, avança `RCV.NXT++`, marca `TCP_PCB_FLAG_EOF` e transmite o ACK.
4. Ao receber o ACK correspondente ao seu FIN (seja em pacote separado ou combinado com retransmissão de FIN), a máquina detecta que ambos os FINs foram trocados e confirmados, transicionando diretamente para `TCP_TIME_WAIT`.
5. Nenhum dos lados cai incorretamente em `CLOSE_WAIT`. O estado `TIME_WAIT` absorve qualquer segmento duplicado e expira deterministicamente.

### 8.1 Decisão Arquitetural: Representação Interna de `CLOSING`
Na RFC 793 clássica, o diagrama de estados define um nó explícito denominado `CLOSING` para a situação em que um FIN remoto é recebido enquanto se está em `FIN_WAIT_1`. No PhotonOS, essa condição é representada internamente de forma canônica pelo par `(state == TCP_FIN_WAIT1, flags & TCP_PCB_FLAG_EOF)`. Esta decisão de projeto é semanticamente idêntica ao estado `CLOSING` da especificação: o PCB reconhece que o par já enviou o FIN, mantém os temporizadores de retransmissão do FIN local ativos e, ao receber a confirmação (`ACK >= SND.NXT`), transiciona pontualmente para `TCP_TIME_WAIT`. Evita-se, assim, a complexidade de um identificador de estado adicional sem qualquer perda de rigor ou conformidade de protocolo no fio.

---

## 9. Ciclo de Vida de Sockets, PCBs e Semântica de Fork/Dup

### 9.1 Refcounting de File Description
- O socket é representado na tabela VFS por um `file_description_t`.
- `fork()` duplica a tabela de descritores e incrementa `fdesc->ref_count++`.
- `dup()` aloca um novo descritor de arquivo apontando para o mesmo `fdesc` e incrementa `ref_count++`.
- `close(fd)` decrementa `ref_count--`. O fechamento do socket e a emissão do FIN só ocorrem na **última referência viva**.
- Testes demonstraram que se o processo pai aceitar uma conexão e bifurcar (`fork()`), o pai pode fechar seu descritor imediatamente enquanto o filho continua servindo dados e realizando o teardown posteriormente sem interrupção.

### 9.2 Isolamento de Listener Socket
- O socket em estado `LISTEN` gerencia a recepção de conexões e a fila de backlog.
- Cada conexão aceita por `accept()` possui um PCB filho completamente independente (`pcb->parent = 0`).
- Fechar o listener (`close(listener_fd)`) cancela apenas conexões incompletas na fila de backlog; as conexões já estabelecidas e ativas permanecem intactas até que seus respectivos descritores sejam explicitamente fechados.

---

## 10. Matriz de Testes e Validação Completa

A suíte dedicada `scripts/test_tcp_phase2d_teardown.py` executa 16 baterias de testes automatizados, combinando testes unitários in-kernel, testes de canal com tráfego real no fio (*wire*) e inspeção profunda de pacotes via arquivo PCAP (`tcp_phase2d_teardown.pcap`).

### 10.1 Resultados da Suíte Phase 2D

```text
=================================================================
      PhotonOS TCP Phase 2D Teardown Suite Results
=================================================================
  UNIT_ACTIVE_CLOSE            : PASS
  UNIT_PASSIVE_CLOSE           : PASS
  UNIT_SIMULTANEOUS_CLOSE      : PASS
  UNIT_DATA_PLUS_FIN           : PASS
  UNIT_DUPLICATE_FIN           : PASS
  UNIT_FIN_RETRANSMISSION      : PASS
  WIRE_PASSIVE_CLOSE           : PASS
  WIRE_ACTIVE_CLOSE            : PASS
  WIRE_EOF_REPEATED            : PASS
  WIRE_FORK_TEARDOWN           : PASS
  WIRE_DUP_TEARDOWN            : PASS
  WIRE_SIMULTANEOUS_CLOSE      : PASS
  FULL_DUPLEX_THEN_CLOSE       : PASS
  PCAP_FIN_ON_WIRE             : PASS
  PCAP_FIN_SEQ_ACCOUNTING      : PASS
  PCAP_CHECKSUMS_VALID         : PASS
=================================================================
Total: 16/16 testes passaram (100% PASS)
```

### 10.2 Regressões das Fases Anteriores
Todos os marcos anteriores foram executados e aprovados sem falhas:
- **TCP Phase 2A (Handshake Ativo & Connect):** 5/5 PASS + Wire checks.
- **TCP Phase 2B.1 (Passive Open & Backlog):** 15/15 PASS + Wire checks.
- **TCP Phase 2B.2A (Receive Path & EOF):** 17/17 PASS (incluindo 5/5 wire tests).
- **TCP Phase 2B.2B (Transmit Path & RTO):** 28/28 PASS.
- **TCP Phase 2C (Flow Control, Persist & Full-Duplex):** 14/14 PASS.
- **Boot Regression:** 10/10 boots consecutivos com sucesso.
- **Subsistemas do Sistema Operacional:** Sinais POSIX, Pipes, VFS, SMP, Fork, Exec e Waitpid 100% operacionais.

### 10.3 Restrições de Build e Tamanho de Imagem
- Flags de compilação: `-Os` mantido rigorosamente.
- Limite máximo do kernel (`KERNEL_MAX_BYTES`): 245.760 bytes (480 setores).
- Tamanho real de `build/photon.bin`: **150.060 bytes**.
- Margem de segurança restante: **95.700 bytes** (~38,9% livre).
