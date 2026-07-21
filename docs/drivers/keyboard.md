# PS/2 Keyboard Driver (IRQ 1)

Este documento especifica detalhadamente a arquitetura, o mapeamento de teclas e o tratamento de estado no driver de teclado PS/2 do PhotonOS.

---

## 1. Visão Geral da Arquitetura

O driver de teclado do PhotonOS gerencia as interrupções de hardware geradas pelo teclado na linha **IRQ 1** (mapeada para a interrupção `0x21` da IDT). O fluxo de processamento de entrada ocorre da seguinte forma:

```
[Hardware Key Press] ──► [IRQ 1] ──► [keyboard_irq_stub (ASM)]
                                            │
                                            ▼
                                [keyboard_handle_scancode]
                                            │ (Gera ASCII via State Machine)
                                            ▼
                                  [keyboard_queue] (Circular)
                                            │
                                            ▼
                                  [console_read (Syscall)]
                                            │
                                            ▼
                                  [Processo Usuário / Shell]
```

---

## 2. Tabelas de Mapeamento (US-QWERTY)

O driver implementa suporte completo ao layout padrão de teclado americano (US-QWERTY) através de duas tabelas de correspondência direta scancode-para-ASCII de 128 bytes:

*   **`keymap_normal[128]`**: Caracteres padrão sem a tecla Shift pressionada:
    *   `[0x28] = '\''` (aspas simples)
    *   `[0x2B] = '\\'` (contra-barra)
    *   `[0x35] = '/'` (barra)
*   **`keymap_shift[128]`**: Caracteres gerados sob ativação da tecla Shift:
    *   `[0x28] = '"'` (aspas duplas)
    *   `[0x2B] = '|'` (barra vertical / pipe)
    *   `[0x35] = '?'` (ponto de interrogação)

---

## 3. Máquina de Estados e Scancodes

O driver opera sobre scancodes do **Scancode Set 1** do controlador de teclado Intel 8042.

### Processamento de Make e Break Codes
*   **Make Code**: Gerado quando uma tecla é pressionada. Consiste no scancode padrão (ex: `0x1E` para 'A').
*   **Break Code**: Gerado quando uma tecla é liberada. Consiste no make code somado a `0x80` (ex: `0x9E` para liberação da tecla 'A').

### Variáveis de Estado
O driver mantém o rastreamento do estado do modificador de teclas:
*   `keyboard_shift`: Setado para 1 quando o make code do Shift Esquerdo (`0x2A`) ou Shift Direito (`0x36`) é processado. Retorna a 0 na recepção do break code correspondente (`0xAA`/`0xB6`).
*   `keyboard_ctrl`: Setado para 1 quando o make code da tecla Ctrl Esquerdo (`0x1D`) é recebido.
*   `keyboard_altgr`: Ativado via códigos estendidos (prefixo `0xE0`).
*   `keyboard_extended`: Flag interna ativada ao ler o prefixo de tecla estendida `0xE0`, instruindo o parser a tratar o próximo byte recebido com semântica estendida.

---

## 4. Teclas de Controle Especiais e IPC

### Pipeline de IPC via Pipe (|)
A ativação da barra vertical (`|`) no scancode `0x2B` + Shift envia o caractere `|` à fila de entrada. Isso permite que o shell interprete redirecionamentos de saída entre processos Ring 3 (ex: `echo "data" | upper`).

### Envio de Sinais (Ctrl+C / SIGINT)
Caso a tecla Ctrl esteja ativa (`keyboard_ctrl == 1`) e a tecla 'C' (make code `0x2E`) seja pressionada, o driver intercepta a sequência e aciona:

```c
static void keyboard_send_sigint(void) {
    task_t *task = keyboard_foreground_task();
    if (task != 0) {
        scheduler_send_signal(task->pid, SIGINT);
    }
}
```

Isso envia imediatamente um sinal `SIGINT` (sinal 2) para o processo rodando em primeiro plano no shell, permitindo a interrupção segura de tarefas Ring 3 travadas sem quebrar a execução global do sistema operacional.

### Suporte a Teclas Adicionais
*   **Tabulação (`\t`)**: O scancode `0x0F` (Tab) é explicitamente capturado e adiciona o caractere `\t` à fila, facilitando a formatação no console do shell.
*   **Backspace (`\b`)**: O scancode `0x0E` adiciona `\b` para remoção de caracteres.
*   **Enter (`\n`)**: O scancode `0x1C` adiciona `\n` para envio e execução de linhas no console.

---

## 5. Fila Circular de Entrada

Os caracteres ASCII validados são enfileirados em um ring buffer estático (`keyboard_queue`):

```c
#define KEYBOARD_QUEUE_SIZE 128
static char keyboard_queue[KEYBOARD_QUEUE_SIZE];
static size_t keyboard_queue_read;
static size_t keyboard_queue_write;
```

*   **Escrita (Push)**: Ocorre na interrupção de hardware. Se a fila estiver cheia, o caractere é descartado silenciosamente.
*   **Leitura (Pop)**: Ocorre na chamada de sistema de leitura do console. Se a fila estiver vazia, o processo de leitura dorme no escalonador até que novos caracteres cheguem.

---

## 6. Depuração e Testes

### Logs no Barramento Serial
Qualquer caractere especial ou alteração de estado modificador gera logs informativos no terminal COM1 (`klog`):
```text
DEBUG: Shift pressed
DEBUG: Special char detected: 0x2B -> '|' (shift=1)
```

### Comandos de Validação Manual
Inicie o sistema via QEMU:
```bash
make run-fat16
```
No shell do PhotonOS, teste as combinações:
*   Pressionar Shift + `\` para verificar a saída do pipe `|`.
*   Digitar aspas simples e duplas: `echo 'teste'` e `echo "teste"`.
*   Executar um loop infinito (ex: `spin`) e interrompê-lo enviando a interrupção Ctrl+C.
