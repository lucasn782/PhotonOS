# 🧮 Cálculo e Validação do Checksum TCP (RFC 793) — PhotonOS v4.2

## 🌐 Visão Geral

O protocolo TCP exige uma verificação de integridade de ponta a ponta que cobre não apenas o cabeçalho TCP e sua carga útil (payload), mas também informações fundamentais do cabeçalho IPv4. O **PhotonOS v4.2** implementa o algoritmo de checksum do TCP em estrita conformidade com a especificação **RFC 793**.

---

## 📐 Pseudo-Cabeçalho IPv4

Para calcular o checksum TCP, é construído logicamente um **Pseudo-Cabeçalho IPv4** de 12 bytes:

```text
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                       Source IPv4 Address                     |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                    Destination IPv4 Address                   |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|      ZERO     |    Protocol   |           TCP Length          |
|     (0x00)    |  (6 / 0x06)   |    (Header + Payload Size)    |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

---

## 🧮 Algoritmo de Cálculo (`tcp_checksum`)

A função `tcp_checksum()` em `src/kernel/tcp.c` realiza o seguinte procedimento:

1. **Soma do Pseudo-Cabeçalho**:
   - Adiciona os dois inteiros de 16 bits do IP de Origem (`src_ip`).
   - Adiciona os dois inteiros de 16 bits do IP de Destino (`dest_ip`).
   - Adiciona o protocolo em 16 bits (`IP_PROTO_TCP = 6`).
   - Adiciona o comprimento total do segmento TCP (`TCP Header Size + Payload Size`).

2. **Soma do Segmento TCP**:
   - Acumula a soma de palavras de 16 bits de todo o cabeçalho TCP e do payload.
   - Caso o segmento possua um número ímpar de bytes, o último byte é preenchido com zero (padded) na posição menos significativa para formar a última palavra de 16 bits.

3. **Dobramento de Vai-um (Carry Fold)**:
   - Os bits excedentes de vai-um (carry) acima de 16 bits (`sum >> 16`) são somados repetidamente aos 16 bits menos significativos (`sum & 0xFFFF`) até que a soma caiba em 16 bits.

4. **Complemento de 1**:
   - O resultado final de 16 bits é invertido bit a bit (`~sum`).

---

## 🔁 Representação do Valor Zero e Validação

- **Envio (`tcp_output`)**: Conforme exigido pela RFC 768/793, se o checksum calculado resultar em `0x0000`, ele é transmitido como `0xFFFF` (representação equivalente em complemento de 1).
- **Recepção (`tcp_input`)**: Ao receber um segmento TCP, a função `tcp_checksum()` é executada sobre todo o pacote incluindo o campo de checksum recebido. Se o pacote estiver íntegro, a soma resulta em `0x0000` (ou `0`). Qualquer valor diferente de 0 indica corrupção e causa o descarte imediato do segmento.
