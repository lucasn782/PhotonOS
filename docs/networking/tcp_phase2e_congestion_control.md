# 🚦 TCP Phase 2E — Controle de Congestionamento (RFC 5681)

## 1. Visão Geral

O marco **TCP Phase 2E** introduz os algoritmos de controle de congestionamento no kernel do **PhotonOS**, em conformidade com as diretrizes da **RFC 5681**:
* **Slow Start** (Partida Lenta)
* **Congestion Avoidance** (Prevenção de Congestionamento)
* **Reação de Perda por Retransmission Timeout (RTO)**

O controle de congestionamento modula a taxa de injeção de pacotes na rede através da variável dinâmica `cwnd` (*congestion window*), garantindo que a transmissão respeite simultaneamente os limites da capacidade da rede e a capacidade de recepção do par (`snd_wnd`).

---

## 2. Estrutura de Estado e Modelo por Conexão

Cada conexão TCP mantém seu próprio estado de congestionamento de forma estritamente isolada na estrutura `struct tcp_pcb` ([include/tcp.h](file:///C:/Users/lucas/OneDrive/Documentos/PhotonOS/include/tcp.h)):

```c
struct tcp_pcb {
    ...
    /* Controle de Congestionamento (RFC 5681 - TCP Phase 2E) */
    uint32_t cwnd;           /* Janela de congestionamento em bytes */
    uint32_t ssthresh;       /* Limiar de Slow Start / Congestion Avoidance */
    uint32_t ca_bytes_acked; /* Acumulador de bytes reconhecidos em Congestion Avoidance */
    ...
};
```

### 2.1 Inicialização Determinística (RFC 5681 Seção 3.1)
Durante a alocação e inicialização do PCB (`tcp_alloc()`):
* **SMSS efetivo:** Definido em `TCP_DEFAULT_MSS = 1460` bytes.
* **Janela Inicial (`IW`):**
  $$\text{IW} = 3 \times \text{SMSS} = 4380 \text{ bytes}$$
  De acordo com a RFC 5681 Seção 3.1 (onde $2190 < \text{SMSS} \le 1095$ permite $3 \times \text{SMSS}$).
* **`ssthresh` Inicial:** Arbitrariamente alto, definido como $65535$ bytes (`TCP_INITIAL_SSTHRESH`).
* **`ca_bytes_acked`:** Inicializado em $0$.

---

## 3. Algoritmos de Crescimento da Janela

### 3.1 Slow Start (RFC 5681 Seção 3.1)
Enquanto $\text{cwnd} < \text{ssthresh}$, a conexão opera em regime de **Slow Start**:
* A cada ACK válido que confirma dados novos ($\text{bytes\_acked} > 0$):
  $$\text{cwnd} \leftarrow \text{cwnd} + \min(\text{bytes\_acked}, \text{SMSS})$$
* Essa formulação suporta ACKs parciais e cumulativos, garantindo crescimento exponencial de até 1 SMSS por pacote confirmado, evitando explosões descontroladas causadas por ACKs excessivamente largos.
* ACKs duplicados (`bytes_acked == 0`) e ACKs fora de ordem são descartados precocemente, não causando crescimento artificial.

### 3.2 Congestion Avoidance (RFC 5681 Seção 3.1)
Quando $\text{cwnd} \ge \text{ssthresh}$, o algoritmo comuta suavemente para **Congestion Avoidance**:
* O crescimento deve ser de aproximadamente $1 \text{ SMSS}$ por RTT ($\approx \text{SMSS} \times \text{SMSS} / \text{cwnd}$).
* Para evitar erros de truncamento em divisão inteira de inteiros pequenos, utiliza-se o acumulador `ca_bytes_acked`:
  $$\text{ca\_bytes\_acked} \leftarrow \text{ca\_bytes\_acked} + \text{bytes\_acked}$$
  $$\text{se } \text{ca\_bytes\_acked} \ge \text{cwnd}:$$
  $$\text{cwnd} \leftarrow \text{cwnd} + \text{SMSS}$$
  $$\text{ca\_bytes\_acked} \leftarrow \text{ca\_bytes\_acked} - \text{cwnd}$$
* Esse mecanismo produz avanço linear consistente, independentemente da granularidade de confirmação dos ACKs recebidos.

---

## 4. Reação à Perda de Segmentos por RTO

Quando o temporizador de retransmissão expira (`tcp_timer_tick()`):
1. **Verificação de Evento Novo:** A redução é aplicada apenas na primeira expiração de um lote de dados (`retransmit_count == 0`), impedindo divisões repetidas espúrias durante o recuo exponencial (exponential backoff).
2. **Cálculo de `FlightSize`:**
   $$\text{FlightSize} = \text{SND.NXT} - \text{SND.UNA}$$
3. **Ajuste de `ssthresh` (RFC 5681 Seção 3.1 Equação 4):**
   $$\text{ssthresh} \leftarrow \max\left(\left\lfloor \frac{\text{FlightSize}}{2} \right\rfloor, 2 \times \text{SMSS}\right)$$
4. **Colapso de `cwnd` (Loss Window):**
   $$\text{cwnd} \leftarrow 1 \times \text{SMSS} = 1460 \text{ bytes}$$
   $$\text{ca\_bytes\_acked} \leftarrow 0$$
5. **Retransmissão:** O primeiro segmento pendente é imediatamente retransmitido e o RTO entra em backoff exponencial.

---

## 5. Integração com o Caminho de Transmissão (TX Path)

O envio de dados no fluxo de saída (`tcp_drain_unsent()`) respeita estritamente o menor limite entre a janela de controle de fluxo anunciada pelo receptor (`snd_wnd`) e a janela de congestionamento calculada (`cwnd`):

$$\text{effective\_window} = \min(\text{snd\_wnd}, \text{cwnd})$$
$$\text{allowed\_to\_send} = \begin{cases} \text{effective\_window} - \text{bytes\_in\_flight}, & \text{se } \text{effective\_window} > \text{bytes\_in\_flight} \\ 0, & \text{caso contrário} \end{cases}$$

### Zero-Window e Persist Timer
* O temporizador de persistência (`persist_timer`) só é ativado se $\text{snd\_wnd} == 0$, preservando a semântica de controle de fluxo de ponta a ponta sem confundir saturação de rede com esgotamento de buffer do receptor.
* ACKs de controle puros (sem dados) nunca são bloqueados por restrições de `cwnd`.

---

## 6. Limitações e Escopo

* **Fast Retransmit e Fast Recovery (RFC 5681 Seção 3.2):** Não foram implementados nesta fase. ACKs duplicados não disparam retransmissão imediata nem ajuste de janela; a detecção e recuperação de perdas são conduzidas pelo temporizador RTO. Esta funcionalidade é candidata a marcos futuros.
* **Syscall `shutdown()`:** Propositalmente mantida fora do escopo desta fase para preservar estabilidade e isolamento de regressões, sendo agendada para a Fase 2F.

---

## 7. Resultados dos Testes Automatizados

### Suíte Dedicada: `scripts/test_tcp_phase2e_congestion_control.py` (16/16 PASS)
| Teste | Descrição | Resultado |
|---|---|---|
| `UNIT_CC_INIT` | Inicialização de `cwnd = 3*MSS`, `ssthresh = 65535`, `ca_bytes_acked = 0` | **PASS** |
| `UNIT_SLOW_START_GROWTH` | Crescimento de `cwnd` por novos ACKs em Slow Start | **PASS** |
| `UNIT_CONGESTION_AVOIDANCE` | Crescimento linear acumulado de 1 MSS por RTT em Congestion Avoidance | **PASS** |
| `UNIT_RTO_LOSS_RECOVERY` | Redução de `ssthresh` e colapso de `cwnd = 1*MSS` após RTO timeout | **PASS** |
| `UNIT_EFFECTIVE_WINDOW` | Respeito à fórmula $\min(\text{snd\_wnd}, \text{cwnd}) - \text{bytes\_in\_flight}$ | **PASS** |
| `UNIT_DUP_ACK_NO_GROWTH` | ACKs duplicados ignorados sem avanço espúrio de `cwnd` | **PASS** |
| `UNIT_RTO_NO_DOUBLE_DROP` | Ausência de rebaixamento duplicado de `ssthresh` em backoff de retransmissão | **PASS** |
| `WIRE_SLOW_START` | Transferência real de 8192 bytes via Slow Start em ambiente de rede QEMU | **PASS** |
| `WIRE_PEER_WINDOW_INTERACTION` | Transmissão modulada conjuntamente por `cwnd` e janela do par | **PASS** |
| `WIRE_LOSS_RECOVERY` | Recuperação e entrega ordenada sob ocorrência de perdas | **PASS** |
| `WIRE_FULL_DUPLEX_CC` | Troca bidirecional simultânea de 16 KiB sob controle de congestionamento | **PASS** |
| `WIRE_TEARDOWN_COMPLIANCE` | Encerramento gracioso ordenado mantido após transferências CC | **PASS** |
| `PCAP_MSS_SEGMENTATION` | Verificação PCAP de segmentação MSS (nenhum payload > 1460 bytes) | **PASS** |
| `PCAP_CWND_FLIGHT_PROGRESSION` | Progressão monotônica dos números de sequência no fio | **PASS** |
| `PCAP_CHECKSUMS_VALID` | Todos os checksums TCP emitidos pelo PhotonOS válidos | **PASS** |
| `PCAP_NO_DUPLICATE_DATA` | Ausência de pacotes espúrios ou corrupção de payload | **PASS** |

### Regressões Globais
* `test_tcp_phase2a.py`: **5/5 PASS**
* `test_tcp_phase2b_passive.py`: **15/15 PASS**
* `test_tcp_phase2b2_rx.py`: **17/17 PASS**
* `test_tcp_phase2b2_tx.py`: **28/28 PASS**
* `test_tcp_phase2c_flow_control.py`: **14/14 PASS**
* `test_tcp_phase2d_teardown.py`: **16/16 PASS**
* `test_10_boots.py`: **10/10 PASS**
* `test_vfs_qemu.py`: **PASS**
* `test_signals_suite.py`: **PASS**
* `test_phase7.py`: **PASS**

---

## 8. Métricas de Compilação e Binário

* **Tamanho do Kernel:** `build/photon.bin`: **150.060 bytes**
* **Limite Máximo:** `KERNEL_MAX_BYTES`: **245.760 bytes**
* **Margem Livre:** **95.700 bytes livres (38,94% de margem)**
* **Flags:** Compilação com `-Os` rigorosamente mantida.
