# TCP Phase 2G — Fast Retransmit & Fast Recovery (RFC 5681)

## 1. Visão Geral e Conformidade Normativa

A **Phase 2G** implementa os algoritmos de **Fast Retransmit** e **Fast Recovery** na pilha TCP do PhotonOS, utilizando a **RFC 5681 (Seções 2 e 3.2)** como referência normativa estrita.

O objetivo fundamental deste mecanismo é permitir que o kernel detecte perdas de segmentos por meio da recepção de **três ACKs duplicados válidos** (*duplicate ACKs*) e retransmita imediatamente o primeiro segmento ainda não confirmado (*oldest unacknowledged segment*), sem depender da expiração do temporizador RTO (*Retransmission Timeout*).

### Escopo e Limitações
- **Modelo Implementado:** RFC 5681 clássico (*Classic Fast Retransmit & Fast Recovery*).
- **Fora do Escopo:**
  - **NewReno (RFC 6582):** Recuperação rápida em ACKs parciais permanece fora do escopo. No modelo clássico, qualquer ACK que reconheça novos dados deflaciona `cwnd` diretamente para `ssthresh` e encerra a recuperação rápida.
  - **SACK (RFC 2018 / RFC 2883):** Selective Acknowledgments não é implementado nesta fase; a contabilidade cumulativa padrão é preservada.
  - **Window Scaling (RFC 7323):** As janelas continuam limitadas a 16 bits (até 65.535 bytes).

---

## 2. Identificação Rigorosa de Duplicate ACKs (RFC 5681 Seção 2)

Nem todo ACK repetido é classificado como duplicate ACK para fins de perda e congestionamento. A pilha implementa uma checagem rigorosa de 5 pontos:

1. **Dados em Trânsito:** A conexão deve possuir dados pendentes de confirmação (`pcb->tx_buf.unacked.head != 0` e `TCP_SEQ_LT(pcb->snd_una, pcb->snd_nxt)`).
2. **Sem Payload:** O segmento recebido deve possuir `payload_len == 0`.
3. **Flags Válidas:** As flags `SYN`, `FIN` e `RST` **não** podem estar presentes no segmento recebido (`flags & (SYN | FIN | RST) == 0`).
4. **Sequência Cumulativa Idêntica:** O campo de confirmação do segmento deve ser exatamente igual ao maior número de sequência cumulativo previamente reconhecido (`TCP_SEQ_EQ(ack, pcb->snd_una)`).
5. **Janela Inalterada:** O valor da janela anunciada pelo peer no segmento recebido deve ser exatamente igual ao valor da janela previamente observado (`hdr.window_size == peer_wnd_before`). Mudanças de janela representam atualizações de controle de fluxo (*window updates*) e não duplicatas de perda.

ACKs antigos (`ack < snd_una`) ou recebidos quando não há dados em voo não alteram o contador `dupacks`.

---

## 3. Algoritmo Fast Retransmit (RFC 5681 Seção 3.2)

Ao receber o **terceiro ACK duplicado válido** consecutivo:

1. **Cálculo de `FlightSize`:**
   O volume de dados em trânsito é determinado por:
   $$\text{FlightSize} = \text{SND.NXT} - \text{SND.UNA}$$
2. **Ajuste de `ssthresh`:**
   $$\text{ssthresh} = \max\left(\left\lfloor\frac{\text{FlightSize}}{2}\right\rfloor, 2 \times \text{SMSS}\right)$$
   Onde $\text{SMSS} = \text{TCP\_DEFAULT\_MSS} = 1460$ bytes.
3. **Retransmissão Imediata:**
   O primeiro segmento da fila de não confirmados (`pcb->tx_buf.unacked.head`) é retransmitido imediatamente, preservando seu número de sequência original, payload e opções. O valor de `SND.NXT` **não** é incrementado, pois não se trata de dados novos.
4. **Reativação do Temporizador RTO:**
   O timer RTO é reiniciado para o segmento retransmitido com o valor base de RTO, garantindo contingência caso a retransmissão também venha a ser perdida.

---

## 4. Algoritmo Fast Recovery (RFC 5681 Seção 3.2)

### 4.1 Entrada em Fast Recovery
Na chegada do 3º ACK duplicado:
- `cwnd` é inicializado como:
  $$\text{cwnd} = \text{ssthresh} + 3 \times \text{SMSS}$$
  (Os $3 \times \text{SMSS}$ compensam os três segmentos que deixaram a rede e geraram os 3 dupACKs).
- A flag `TCP_PCB_FLAG_FAST_RECOVERY` é ativada no PCB.
- O ponto de recuperação é gravado: `pcb->recover = pcb->snd_nxt`.
- `ca_bytes_acked` é reinicializado para `0`.

### 4.2 ACKs Duplicados Adicionais (> 3 dupACKs)
Para cada dupACK adicional recebido enquanto `TCP_PCB_FLAG_FAST_RECOVERY` estiver ativa:
- `dupacks` é incrementado.
- `cwnd` é inflacionado:
  $$\text{cwnd} = \text{cwnd} + \text{SMSS}$$
- Se permitido pela janela efetiva $\min(\text{snd\_wnd}, \text{cwnd}) - \text{bytes\_in\_flight}$, `tcp_drain_unsent()` pode transmitir novos segmentos que estejam retidos na fila de não enviados.

### 4.3 Saída de Fast Recovery
Quando um ACK válido reconhece novos dados (`TCP_SEQ_GT(ack, pcb->snd_una)`):
- `cwnd` é desinflacionado (*deflate*):
  $$\text{cwnd} = \text{ssthresh}$$
- A flag `TCP_PCB_FLAG_FAST_RECOVERY` é limpa.
- O contador `dupacks` é reinicializado para `0`.
- O ACK de saída **não** sofre incremento adicional de Slow Start ou Congestion Avoidance (`is_recovery_exit = 1` em `tcp_tx_ack_received_locked_ext()`), prevenindo crescimento duplo artificial de `cwnd`.
- A partir dos ACKs seguintes, a conexão retoma a evolução ordinária de Congestion Avoidance ou Slow Start.

---

## 5. Interação com RTO e Congestion Control da Phase 2E

1. **RTO durante Fast Recovery:**
   Se o temporizador RTO expirar enquanto a conexão estiver em Fast Recovery:
   - A recuperação rápida é imediatamente abortada (`TCP_PCB_FLAG_FAST_RECOVERY` limpa, `dupacks = 0`).
   - Aplica-se a regra de timeout da Phase 2E:
     $$\text{ssthresh} = \max\left(\frac{\text{FlightSize}}{2}, 2 \times \text{SMSS}\right)$$
     $$\text{cwnd} = 1 \times \text{SMSS}$$
   - O RTO aplica backoff exponencial ordinário.
2. **Preservação de Locks e Envio sem Preempção:**
   A montagem da retransmissão de Fast Retransmit ocorre sob `pcb->lock` para proteger o buffer TX e o estado do PCB, mas a chamada a `net_send_ipv4()` é executada **após** a liberação de `pcb->lock`, utilizando o buffer estático `tcp_fast_rexmit_tx` protegido por `tcp_fast_rexmit_lock`. Isso elimina qualquer risco de inversão de prioridade ou deadlock com os locks de baixo nível do subsistema de rede.

---

## 6. Resultados de Testes e Validação Completa

A suíte automatizada `scripts/test_tcp_phase2g_fast_retransmit.py` implementa e valida todos os 32 cenários normativos da Phase 2G:

```text
====================================================================
      PhotonOS TCP Phase 2G Fast Retransmit / Recovery Results
====================================================================
  ACK_CLASS_VALID_UNACKED           : PASS
  ACK_CLASS_NO_FLIGHT_DATA          : PASS
  ACK_CLASS_NEW_DATA_ACK            : PASS
  ACK_CLASS_PAYLOAD_REJECT          : PASS
  ACK_CLASS_WINDOW_CHANGE           : PASS
  ACK_CLASS_FLAGS_REJECT            : PASS
  ACK_CLASS_POST_RECOVERY           : PASS
  ACK_CLASS_WRAPAROUND              : PASS
  FAST_REXMIT_FIRST_DUPACK          : PASS
  FAST_REXMIT_SECOND_DUPACK         : PASS
  FAST_REXMIT_THIRD_DUPACK          : PASS
  FAST_RECOVERY_SSTHRESH            : PASS
  FAST_RECOVERY_CWND_INIT           : PASS
  FAST_RECOVERY_CWND_INFLATION      : PASS
  FAST_RECOVERY_EXIT_NEW_ACK        : PASS
  FAST_RECOVERY_CWND_DEFLATION      : PASS
  FAST_RECOVERY_STATE_RESET         : PASS
  FAST_RECOVERY_NO_DOUBLE_GROWTH    : PASS
  RTO_BEFORE_THIRD_DUPACK           : PASS
  RTO_DURING_FAST_RECOVERY          : PASS
  CLASSIC_PARTIAL_ACK_LIMITATION    : PASS
  ZERO_WINDOW_PERSIST_INTERACTION   : PASS
  INDEPENDENT_CONGESTION_STATE      : PASS
  WIRE_EFFECTIVE_WND_TX             : PASS
  WIRE_NO_DUPLICATE_DATA            : PASS
  WIRE_FULL_DUPLEX_RECOVERY         : PASS
  WIRE_TEARDOWN_AFTER_RECOVERY      : PASS
  WIRE_SHUTDOWN_WR_RECOVERY         : PASS
  WIRE_FORK_DUP_COMPATIBILITY       : PASS
  PCAP_CHECKSUMS_VALID              : PASS
  PCAP_REXMIT_FIRST_UNACKED         : PASS
  PCAP_REXMIT_PRESERVES_SEQ         : PASS
====================================================================
Total: 32/32 testes passaram (100% PASS)
>>> ALL TCP PHASE 2G FAST RETRANSMIT & FAST RECOVERY TESTS PASSED! <<<
====================================================================
```

### Regressões de Milestones Anteriores
- **Phase 2F (Shutdown & Half-Close):** 23/23 PASS (100%)
- **Phase 2E (Congestion Control):** 16/16 PASS (100%)
- **Phase 2D (Teardown & FIN):** 16/16 PASS (100%)
- **Phase 2C (Flow Control & Persist):** 14/14 PASS (100%)
- **Phase 2B.2B (TX Path):** 28/28 PASS (100%)
- **Phase 2B.2A (RX Path):** 17/17 PASS (100%)
- **Phase 2B.1 (Passive Open):** PASS (100%)
- **Phase 2A (Active Open & Handshake):** PASS (100%)
- **Estabilidade do Sistema (10 Boots):** 10/10 PASS (100%)
- **Suíte VFS / EXT2 / ATA / Shell:** PASS (100%)
- **Suíte de Sinais / Pipes / SMP:** PASS (100%)
- **Phase 7 General System Integration:** PASS (100%)

### Orçamento de Tamanho do Kernel
- **Limite Máximo (`KERNEL_MAX_BYTES`):** 245.760 bytes
- **Tamanho Final (`build/photon.bin`):** 162.452 bytes
- **Margem Livre Restante:** 83.308 bytes (~33,9% de folga)
