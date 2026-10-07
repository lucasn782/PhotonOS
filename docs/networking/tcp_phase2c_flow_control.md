# 🌊 TCP Phase 2C — Controle de Fluxo, Persist Timer e Full-Duplex Simultâneo

## 1. Visão Geral

O milestone **TCP Phase 2C** consolida o controle de fluxo orientado a janelas (RFC 793), a proteção contra deadlocks de janela zero (*persist timer*), o suporte completo a tráfego *full-duplex* simultâneo em canais bidirecionais e a blindagem de sincronização e preempção concorrente no kernel do **PhotonOS**.

---

## 2. Janela de Recepção Anunciada (Advertised Receive Window)

### 2.1 Cálculo Dinâmico de `rcv_wnd`
A janela de recepção anunciada no fio reflete fielmente a capacidade livre real do buffer circular de recepção (`tcp_rx_buffer_t`, capacidade de 8192 bytes):

$$\text{free\_space} = \text{capacity} - \text{used}$$

No PhotonOS, `rcv_wnd` e o campo `window_size` do cabeçalho TCP emitido são calculados dinamicamente sob posse de `pcb->lock`:
- Quando o buffer está vazio (`used == 0`), a janela anunciada é a capacidade máxima (8192 bytes).
- À medida que segmentos chegam e são armazenados no buffer circular antes de o processo em espaço de usuário consumi-los, a janela anunciada encolhe progressivamente (*window shrink*).
- Se o buffer atingir a capacidade total (8192 bytes), a janela anunciada reduz-se exatamente a **0** (*zero window*), instruindo o transmissor remoto a suspender novos envios de dados normais.

### 2.2 Reabertura de Janela (Window Reopening)
Quando a aplicação em Ring 3 consome dados através de `sys_recv()`, espaço é liberado no buffer circular. Se a janela anterior estava baixa ou fechada, o kernel emite imediatamente um pacote de atualização de janela (*window update ACK*) com o novo valor de `rcv_wnd`, destravando o fluxo de transmissão do par.

---

## 3. Janela de Transmissão do Par (Peer Window Enforcement)

### 3.1 Rastreamento de `snd_wnd` e Regras RFC 793
O transmissor local rastreia a janela oferecida pelo receptor remoto através dos campos:
- `snd_wnd`: Janela anunciada pelo par (convertida da ordem de rede).
- `snd_wl1`: Número de sequência do segmento que atualizou a janela pela última vez.
- `snd_wl2`: Número de reconhecimento do segmento que atualizou a janela pela última vez.

A atualização de `snd_wnd` segue estritamente a especificação do RFC 793:
Uma nova janela é aceita se:
1. `sequence > snd_wl1`, ou
2. `sequence == snd_wl1` e `acknowledgement >= snd_wl2`, ou
3. O valor de `window_size` mudou explicitamente em relação a `snd_wnd` atual em um ACK válido.

### 3.2 Imposição de Janela e Bytes em Voo (Bytes in Flight)
Antes de despachar dados da fila `unsent`, o subsistema calcula os dados já enviados que aguardam confirmação:

$$\text{bytes\_in\_flight} = \text{SND.NXT} - \text{SND.UNA}$$
$$\text{remaining\_wnd} = \begin{cases} \text{snd\_wnd} - \text{bytes\_in\_flight}, & \text{se } \text{snd\_wnd} > \text{bytes\_in\_flight} \\ 0, & \text{caso contrário} \end{cases}$$

O tamanho do próximo segmento transmitido é limitado a:

$$\text{send\_len} = \min(\text{segment\_length}, \text{remaining\_wnd}, \text{TCP\_DEFAULT\_MSS})$$

Se o segmento na fila `unsent` for maior que `send_len`, o kernel realiza o fatiamento (*slicing*) do segmento: aloca um segmento de corte para os primeiros `send_len` bytes, avança `seg->offset` e diminui `seg->length` do segmento restante em `unsent`.

---

## 4. Persist Timer e Probes de Zero-Window

### 4.1 Problema da Janela Zero
Se o receptor anunciar uma janela de 0 bytes e o pacote subsequente de reabertura de janela for perdido na rede, transmissor e receptor entrariam em deadlock indefinido: o transmissor esperando a janela abrir, e o receptor esperando novos dados.

### 4.2 Mecanismo de Probing
Para resolver essa condição, o PhotonOS implementa o **Persist Timer**:
1. Quando `snd_wnd == 0` e existem dados pendentes na fila `unsent`, o temporizador de persistência é armado (`TCP_PCB_FLAG_TIMER_PERSIST`).
2. O intervalo inicial é definido como `TCP_PERSIST_TICKS_DEFAULT` (100 ticks).
3. Ao expirar (`tcp_timer_tick()`), o kernel transmite um pacote de sondagem (*zero-window probe*) com flag `ACK`, `seq = snd_nxt - 1` e `len = 0` (ou 1 byte de dados não confirmados).
4. O temporizador sofre *backoff exponencial* dobrando a cada disparo até o teto `TCP_MAX_PERSIST_TICKS` e limite de tentativas `TCP_MAX_PERSIST_PROBES`.
5. Ao receber a resposta com janela reaberta (`snd_wnd > 0`), o persist timer é desarmado e a rotina `tcp_drain_unsent()` é disparada para retomar o fluxo de dados represados.

---

## 5. Arquitetura de Concorrência e Sincronização

Durante a validação de fluxos interativos de echo e tráfego concorrente, foram identificados e resolvidos pontos críticos de contenção e preempção entre chamadas de sistema e o escalonador:

### 5.1 Preempção e `mutex_lock_preemptible()`
- **Contexto:** No x86_64, a transição para Ring 0 via instrução `syscall` limpa automaticamente o bit de interrupções (`IF`) em RFLAGS de acordo com a máscara `IA32_FMASK` (`SYSCALL_FMASK`).
- **Problema:** Quando uma tarefa em syscall tentava adquirir um mutex já ocupado por outra CPU ou thread com `IF=0`, a rotina padrão de mutex caía em spinloop contínuo (`pause`) sem conseguir adormecer na fila de espera (`TASK_WAIT_MUTEX`), porque `interrupts_are_enabled()` retornava falso. Isso impedia a entrega do tick do temporizador e a troca de contexto, congelando a CPU.
- **Solução:** Implementação de `mutex_lock_preemptible()`. Para tarefas escalonáveis (`current != 0 && current->pid != 0`), a função temporariamente reabilita interrupções (`sti`) antes de disputar o lock e restaura o estado original de `IF` ao retornar. Isso permite que tarefas esperando por locks em chamadas como `sys_recv()` sejam preemptadas normalmente, permitindo que a tarefa proprietária do lock progrida e libere o recurso.

### 5.2 Delimitação do Escopo de `sock->mutex`
- **Problema:** Em operações como `sys_send()`, reter `sock->mutex` durante toda a execução de `tcp_send()` bloqueava processos filhos que tentavam realizar `sys_recv()` concorrentemente no mesmo descritor compartilhado pós-`fork()`.
- **Solução:** `sys_send()` adquire `sock->mutex` apenas para validar a integridade do socket e extrair o ponteiro seguro do PCB (`pcb = sock->tcp`), liberando o lock do socket antes de chamar `tcp_send()`. O PCB possui seu próprio mutex (`pcb->lock`) para garantir a serialização atômica de suas filas internas.
- **Teardown Seguro:** Em `socket_vfs_close()`, o ponteiro `sock->tcp` é dissociado sob `sock->mutex` e o lock é liberado antes de invocar `tcp_socket_destroy(pcb)`, eliminando qualquer possibilidade de inversão de locks ou deadlock durante o fechamento de sockets compartilhados.

### 5.3 Transmissão de Rede Fora de Locks
Em conformidade com a arquitetura do PhotonOS, nenhuma transmissão de rede (`net_send_ipv4()`) é realizada mantendo `pcb->lock` ou `tcp_pcbs_lock`. Os pacotes são construídos e copiados para buffers locais sob o lock do PCB, o lock é liberado, e o pacote é despachado via DMA na interface de rede.

---

## 6. Full-Duplex Simultâneo

O suporte a full-duplex simultâneo permite que processos independentes ou bifurcados via `fork()` realizem transmissões e recepções simultâneas de grandes volumes de dados no mesmo fluxo TCP:
- Testado e validado com transferência simultânea de **16 KiB** (16.384 bytes) em cada direção:
  - Host enviando padrão gerado de 16.384 bytes para o guest.
  - Processo pai no guest transmitindo padrão gerado de 16.384 bytes para o host.
  - Processo filho no guest recebendo simultaneamente os 16.384 bytes do host.
- Resultados validados com correspondência estrita de 100% dos bytes (`host_sent=16384/16384`, `fd_received=16384/16384`) e integridade de padrões sem perda de pacotes, retries infinitos ou buffers corrompidos.

---

## 7. Suíte de Testes Automatizados e Validação PCAP

A suíte [`test_tcp_phase2c_flow_control.py`](../../scripts/test_tcp_phase2c_flow_control.py) valida de forma automatizada 14 critérios estritos:

| Identificador de Teste | Descrição da Verificação | Status |
| :--- | :--- | :---: |
| `UNIT_ADVERTISED_WINDOW` | Teste in-kernel de cálculo de janela anunciada (`capacity - used`, zero window e reopening) | **PASS** |
| `UNIT_PEER_WINDOW` | Teste in-kernel de imposição de `snd_wnd`, cálculo de `bytes_in_flight` e fatiamento (< MSS) | **PASS** |
| `UNIT_PERSIST_TIMER` | Teste in-kernel de armamento em zero-window, backoff exponencial e desarme pós-reabertura | **PASS** |
| `FLOW_RX_SATURATION` | Saturação controlada do buffer RX (8192 bytes recebidos sem leitura imediata) | **PASS** |
| `FLOW_SEND_LIMITED` | Transmissão guest limitada e fatiada de acordo com a janela anunciada pelo par (4000 bytes) | **PASS** |
| `FLOW_ECHO_INTERACTIVE` | Fluxo interativo de echo com alternância dinâmica entre envio e recepção | **PASS** |
| `FULL_DUPLEX_SIMULTANEOUS` | Troca bidirecional simultânea de 16 KiB (16.384 bytes) em cada sentido | **PASS** |
| `FULL_DUPLEX_PATTERN_MATCH` | Validação de integridade byte-a-byte dos dados transmitidos e recebidos | **PASS** |
| `PCAP_INITIAL_WINDOW` | Janela inicial anunciada em conformidade com o buffer físico ($\le 8192$ bytes) | **PASS** |
| `PCAP_WINDOW_DYNAMICS` | Observação de flutuações dinâmicas de janela capturadas no arquivo PCAP | **PASS** |
| `FLOW_WINDOW_SHRINK` | Redução comprovada no fio da janela anunciada sob saturação do RX buffer ($< 4096$) | **PASS** |
| `FLOW_WINDOW_REOPEN` | Reabertura comprovada no fio da janela após consumo em Ring 3 ($\ge 6000$) | **PASS** |
| `PCAP_FLOW_CHECKSUMS` | Checksums válidos e não-nulos em todos os segmentos emitidos pelo guest | **PASS** |
| `PCAP_MSS_COMPLIANCE` | Conformidade estrita com o MSS: nenhum segmento guest ultrapassa 1460 bytes de payload | **PASS** |
