# TCP Phase 2B.2B — TX Path, `send()` e ACK de Dados

## Escopo

Esta fase acrescenta transmissão de payload TCP a uma conexão já
`ESTABLISHED`. Ela não declara TCP confiável completo: não há janela deslizante
real, controle de congestionamento, SACK, window scaling nem uma etapa de
validação de full-duplex simultâneo.

## Auditoria da Phase 2B.2A

O PCB em `include/tcp.h` já continha a base que esta fase reutiliza:

- `iss` e `irs`: números iniciais de sequência local e remoto;
- `snd_una`: primeiro byte local ainda não confirmado;
- `snd_nxt`: próximo número de sequência local a transmitir;
- `rcv_nxt`: próximo byte remoto esperado;
- `rx_buf`: ring buffer de 8192 bytes usado por `recv()`.

`tcp_input()` já validava o checksum, aceitava RX ordenado, descartava
duplicados/fora de ordem e emitia ACK. `tcp_socket_notify()` acorda tarefas
que esperam pelo socket através do escalonador. A fila `send_queue` existente
era somente um suporte parcial para segmentos; ela foi consolidada no estado
TX explícito abaixo, evitando uma estrutura concorrente paralela.

## Arquitetura

```text
Ring 3 send(fd, buffer, len, 0)
        |
        +-- vmm_validate_user_ptr(buffer, len, 0)
        |
        v
sys_send() -> tcp_send()
        |
        v
pcb->tx_buf.unsent -> pcb->tx_buf.unacked
        |
        v
TCP (MSS 1460) -> net_send_ipv4() -> e1000
        |
        v
peer ACK -> tcp_input() -> SND.UNA -> libera unacked
```

Cada PCB possui `tcp_tx_buffer_t`, com capacidade total de 8192 bytes. Ele
separa `unsent` de `unacked`; um segmento passa para `unacked` imediatamente
antes de a chamada a IPv4 ocorrer. Assim um ACK que chegue logo após a
liberação de `pcb->lock` sempre encontra a cópia do payload que ele confirma.
Cada entrada preserva sequência, tamanho, offset, payload alocado no kernel,
timestamp de envio e contador de retransmissões.

## `send()`

`SYS_SEND` é 55; a ulibc expõe `send(int fd, const void *buf, size_t len,
int flags)`. A syscall aceita somente um socket `SOCK_STREAM`/`IPPROTO_TCP`
ativo com PCB em `TCP_ESTABLISHED` e `flags == 0`.

- `len == 0` retorna 0 e não emite segmento vazio;
- ponteiro nulo, endereço inválido ou intervalo que cruza o limite de Ring 3
  retorna -1 antes de qualquer cópia;
- listener, socket fechado, descritor inválido e socket não TCP retornam -1;
- o payload é copiado para memória do kernel, nunca fica referenciado no
  espaço de usuário.

A política desta fase é deliberadamente não bloqueante. Há espaço? `send()`
aceita e transmite o máximo que couber. Para uma solicitação maior que o
buffer, pode retornar uma escrita parcial positiva; se não houver espaço
disponível, retorna -1 e o chamador deve tentar novamente. Não há busy wait.

## Sequência, segmentação e ACK

`TCP_DEFAULT_MSS` é 1460. Para cada segmento de tamanho `N`, o cabeçalho usa
`SEQ = SND.NXT`; após ele entrar em `unacked`, `SND.NXT += N` com aritmética
natural de `uint32_t`. Os comparadores `TCP_SEQ_LT/LE/GT/GE/EQ` são usados no
caminho de reconhecimento para manter a semântica modular.

Segmentos de dados usam `ACK|PSH`, `ack_num = rcv_nxt`, a janela anunciada
atual e checksum TCP com pseudo-header IPv4. `tcp_send()` monta o pacote sob
`pcb->lock`, solta o lock e só então chama `net_send_ipv4()`.

No ACK recebido:

- `ACK == SND.UNA`: duplicado; não avança estado;
- `ACK < SND.UNA`: antigo; é ignorado;
- `SND.UNA < ACK <= SND.NXT`: atualiza `SND.UNA`, libera segmentos completos
  e ajusta o primeiro segmento parcialmente reconhecido;
- `ACK > SND.NXT`: inválido; não altera fila nem sequência.

## RTO e limpeza

`TCP_DATA_RTO_TICKS_DEFAULT` é separado semanticamente de
`TCP_CONNECT_TIMEOUT_TICKS`: o primeiro controla a retransmissão de dados e o
segundo limita o handshake. Enquanto `unacked` não estiver vazio, o timer
retransmite o segmento mais antigo com o mesmo `SEQ`, aplica backoff simples
até 1000 ticks e limita a `TCP_MAX_DATA_RETRIES` (5). Excedido o limite, a
conexão passa a `CLOSED`, recebe a flag de reset, libera as filas TX e acorda
os usuários do socket.

`RST` recebido, destruição do PCB e fechamento final do socket também liberam
as filas `unsent` e `unacked`. Como o fechamento ordenado por FIN ainda não
existe, o `close()` local remove a conexão da demultiplexação e libera seu
estado local; a sinalização de encerramento ordenado fica para uma fase
posterior. `fork()` e `dup()` compartilham a file description já existente;
portanto o PCB/TX state só é destruído quando a última referência
VFS é fechada. Durante `send()` e escrita VFS, `sock->mutex` permanece retido
até `tcp_send()` terminar de usar o PCB. O fechamento final toma esse mesmo
lock antes de destruir o PCB, eliminando a janela de use-after-free entre a
validação do socket e a transmissão.

## Locks

- `tcp_pcbs_lock` protege a lista global de PCBs;
- `pcb->lock` protege estado, sequências, filas e timers de uma conexão;
- `tcp_deferred_tx_lock` protege o armazenamento estático usado pelo timer;
- nenhuma chamada a `net_send_ipv4()` ocorre com `tcp_pcbs_lock` ou
  `pcb->lock` retido. O lock de ownership do socket pode permanecer retido
  durante a chamada; callbacks TCP não o adquirem e o fechamento final usa a
  mesma ordem `sock -> pcb`.

O ACK e o timer serializam contra `send()` por `pcb->lock`. A cópia local do
pacote já está completa antes de desbloquear, e os dados de retransmissão
continuam pertencendo à fila TX até um ACK válido ou limpeza terminal.

## Testes

`scripts/test_tcp_phase2b2_tx.py` exercita buffer/ACK/RTO no boot, envio de 1
byte, string, 1024 bytes, segmentação MSS, 8192 e 16384 bytes, múltiplas
conexões, `fork`, `dup`, casos negativos e inspeção PCAP de DATA/ACK. O caso
de 16384 bytes obriga o aplicativo a lidar com escrita parcial.

Para validação completa, execute em um ambiente com toolchain ELF x86_64 e
QEMU instalados:

```text
make clean && make && make fat16-disk
python3 scripts/test_10_boots.py
python3 scripts/test_signals_suite.py
python3 scripts/test_vfs_qemu.py
python3 scripts/test_tcp_phase2a.py
python3 scripts/test_tcp_phase2b_passive.py
python3 scripts/test_tcp_phase2b2_rx.py
python3 scripts/test_tcp_phase2b2_tx.py
```

O PCAP da última suíte deve demonstrar, para ao menos um segmento, `ACK = SEQ
+ LEN`; retransmissões, se induzidas, mantêm o mesmo `SEQ`.

## Limitações pendentes

- integração full-duplex como objetivo formal da Phase 2B.2C;
- sliding window e uso de `snd_wnd` para limitação de emissão;
- congestion control, slow start, AIMD e fast retransmit;
- SACK, ECN, window scaling, TLS e encerramento TCP ordenado completo.
