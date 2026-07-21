# PS/2 Mouse Driver (IRQ 12)

Este documento descreve a implementação do driver de mouse PS/2 no kernel do PhotonOS, abordando a inicialização do chip 8042, processamento de pacotes de dados, limites de tela e integração com o cursor gráfico.

---

## 1. Visão Geral e Registradores

O mouse PS/2 é tratado como um dispositivo auxiliar controlado pelo processador de teclado **Intel 8042**. As interrupções de hardware são sinalizadas na linha **IRQ 12** (mapeada para o vetor `0x2C` da IDT).

O driver interage com o hardware através de portas de E/S de 8 bits:
*   **Porta de Dados (`0x60`)**: Usada para ler dados de movimento e botões do mouse ou enviar argumentos de comandos.
*   **Porta de Comando/Status (`0x64`)**: Usada para interrogar o estado do controlador ou enviar comandos de controle de barramento.

---

## 2. Pipeline de Inicialização (`mouse_init`)

Para ativar o streaming de dados do mouse PS/2:
1.  **Habilitação da Porta Auxiliar**: Envia o comando `0xA8` para a porta `0x64`.
2.  **Ativação do IRQ 12**:
    *   Lê o byte de configuração atual do controlador (comando `0x20` em `0x64`, leitura em `0x60`).
    *   Seta o Bit 1 (habilita interrupção do dispositivo auxiliar IRQ 12).
    *   Limpa o Bit 5 (mouse clock disable, ativando a linha de clock do mouse).
    *   Grava as novas configurações enviando o comando `0x60` para a porta `0x64` e o novo byte modificado em `0x60`.
3.  **Configurações Padrão**: Envia o comando `0xF6` para o mouse e aguarda a resposta de confirmação (ACK = `0xFA`).
4.  **Habilitação de Streaming**: Envia o comando `0xF4` para o mouse para permitir o envio contínuo de pacotes físicos na interrupção, aguardando outro ACK `0xFA`.

---

## 3. Estrutura do Pacote de Dados PS/2

O mouse envia dados em pacotes de **3 bytes** contínuos. O driver coleta esses bytes no vetor `mouse_packet[3]` de forma incremental controlada pela variável `mouse_cycle` (ciclos 0 a 2).

### Estrutura dos Bytes

| Byte | Bit 7 | Bit 6 | Bit 5 | Bit 4 | Bit 3 | Bit 2 | Bit 1 | Bit 0 |
| :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: |
| **0** | Y overflow | X overflow | Y sign | X sign | Sempre 1 | Botão Meio | Botão Dir | Botão Esq |
| **1** | Delta X (dx) de 8 bits | | | | | | | |
| **2** | Delta Y (dy) de 8 bits | | | | | | | |

*   **Validação de Sincronismo**: O primeiro byte do pacote (ciclo 0) **deve** possuir o Bit 3 setado em `1`. Se o driver receber um byte com o bit 3 zerado no ciclo 0, considera que houve perda de sincronia do barramento e descarta o byte silenciosamente até encontrar um byte de início válido.

---

## 4. Processamento de Movimentos e Clamping

Quando o terceiro byte do ciclo é recebido, o driver processa a entrada:
1.  **Extensão de Sinal**:
    *   Se o Bit 4 do primeiro byte estiver ativo (sinal X negativo), o delta X (`dx`) é estendido para 32 bits negativos (`dx |= 0xFFFFFF00`).
    *   Se o Bit 5 do primeiro byte estiver ativo (sinal Y negativo), o delta Y (`dy`) é estendido.
2.  **Atualização de Coordenadas Globais**:
    *   `mouse_x = mouse_x + dx`
    *   `mouse_y = mouse_y - dy` (a coordenada Y na tela é invertida, crescendo para baixo, enquanto a variação física cresce para cima).
3.  **Strict Clamping (Limites de Tela)**:
    *   O driver obtém a resolução atual da tela via `video_width()` e `video_height()`.
    *   Garante que as coordenadas fiquem rigidamente dentro dos limites da tela física (ex: de $0$ a $\text{width}-1$).
4.  **Redesenho**: Aciona `video_swap_buffers()` para redesenhar a seta na nova posição imediatamente.
5.  **EOI**: Envia sinal de EOI ao controlador PIC via `pic_send_eoi(12)`.
