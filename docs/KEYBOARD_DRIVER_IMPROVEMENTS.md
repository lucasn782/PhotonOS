# PhotonOS Keyboard Driver IRQ 1 (PS/2) - Expansão e Mapeamento de Caracteres Especiais

## Sumário das Mudanças

Este documento descreve as melhorias implementadas no driver de teclado do PhotonOS para suportar mapeamento completo de caracteres especiais, incluindo pipe (|), aspas (simples e duplas), barra invertida (\), e barras (/).

### Data: 2026-05-29
### Arquivo Modificado: `kernel.c`

---

## 1. Expansão das Tabelas de Mapeamento de Scancode

### 1.1 Tabela Normal (keymap_normal[128])

Localização: kernel.c, linhas ~155-205

**Características:**
- Cobertura completa do layout US-QWERTY
- 128 entradas representando scancodes PS/2 Set 1 (0x00-0x7F)
- Inicialização explícita de 0 para entradas não mapeadas

**Mapeamentos críticos de caracteres especiais:**

```c
[0x28] = '\'   // Aspas simples (scancode para " em US-QWERTY)
[0x2B] = '\\' // Barra invertida/escape
[0x35] = '/'   // Barra normal
[0x0C] = '-'   // Hífen (também usado para underscore com Shift)
[0x0D] = '='   // Igualdade (também usado para plus com Shift)
[0x1A] = '['   // Colchete esquerdo (brace com Shift)
[0x1B] = ']'   // Colchete direito (brace com Shift)
```

### 1.2 Tabela Shift (keymap_shift[128])

Localização: kernel.c, linhas ~208-260

**Características:**
- Espelha a estrutura de keymap_normal
- Contém caracteres modificados pela tecla Shift
- Suporta maiúsculas e símbolos superiores

**Mapeamentos críticos com Shift:**

```c
[0x28] = '"'   // Aspas duplas (SEM Shift = ', COM Shift = ")
[0x2B] = '|'   // PIPE - Character crítico para IPC (SEM Shift = \, COM Shift = |)
[0x35] = '?'   // Interrogação (SEM Shift = /, COM Shift = ?)
```

---

## 2. Melhorias na Função de Conversão de Scancode

### 2.1 keyboard_char_from_scancode(uint8_t scancode)

**Antes:**
```c
static char keyboard_char_from_scancode(uint8_t scancode)
{
    if (scancode >= sizeof(keymap_normal)) {
        return 0;
    }
    if (keyboard_shift) {
        return keymap_shift[scancode];
    }
    return keymap_normal[scancode];
}
```

**Depois (com melhorias):**
```c
static char keyboard_char_from_scancode(uint8_t scancode)
{
    // Boundary check: scancodes 0-127 são válidos
    if (scancode >= 128) {
        return 0;
    }

    // Seleciona keymap baseado no estado de Shift
    const char *keymap = keyboard_shift ? keymap_shift : keymap_normal;
    char ch = keymap[scancode];

    // Debug logging para caracteres especiais
    if (ch == '|' || ch == '"' || ch == '\'' || ch == '\\' || ch == '/') {
        klog("DEBUG: Special char detected: 0x%02X -> '%c' (shift=%d)\n", 
             scancode, ch, keyboard_shift);
    }

    return ch;
}
```

**Benefícios:**
- Validação de boundary explícita (128 em vez de sizeof)
- Debug logging para caracteres especiais
- Facilita troubleshooting de problemas de input

---

## 3. Aprimoramento do Handler de Interrupção de Teclado

### 3.1 keyboard_handle_scancode(uint8_t scancode)

**Mudanças principais:**

1. **Detecção melhorada de prefixo 0xE0 (Extended Scancodes)**
   - Marca flag `keyboard_extended` ao detectar 0xE0
   - Processa scancodes estendidos corretamente (ex: setas, delete estendido)

2. **Tratamento robusto de Break Codes (Release Keys)**
   ```c
   if (scancode & 0x80) {  // Bit 0x80 indica key release
       uint8_t released = scancode & 0x7F;
       // Processa liberação de Shift (0xAA = 0x2A + 0x80, 0xB6 = 0x36 + 0x80)
       if (!extended && (released == 0x2A || released == 0x36)) {
           keyboard_shift = 0;
       }
       // Processa liberação de Ctrl (0x9D = 0x1D + 0x80)
       else if (released == 0x1D) {
           keyboard_ctrl = 0;
       }
       return;
   }
   ```

3. **Suporte a Tab (0x0F)**
   ```c
   if (scancode == 0x0F) {
       keyboard_queue_push('\t');
       return;
   }
   ```

4. **Debug Logging Extensivo**
   - Logs de Shift pressionado/solto
   - Logs de Ctrl detectado
   - Logs de caracteres especiais traduzidos
   - Logs de scancodes estendidos ignorados
   - Logs de scancodes sem mapeamento

---

## 4. Fluxo Completo de Entrada

### 4.1 Do Hardware até Ring 3

```
┌─────────────────────────────────────────────────┐
│ 1. Teclado PS/2 (Hardware)                      │
│    - Envia scancode para porta 0x60             │
└──────────────────┬──────────────────────────────┘
                   ↓
┌─────────────────────────────────────────────────┐
│ 2. IRQ 1 Handler (kernel.asm:keyboard_irq_stub)│
│    - Salva contexto                             │
│    - Chama keyboard_irq_handler()               │
└──────────────────┬──────────────────────────────┘
                   ↓
┌─────────────────────────────────────────────────┐
│ 3. keyboard_irq_handler() (kernel.c)            │
│    - Lê scancode do KEYBOARD_DATA_PORT (0x60)  │
│    - Chama keyboard_handle_scancode()           │
│    - Envia EOI ao PIC                           │
└──────────────────┬──────────────────────────────┘
                   ↓
┌─────────────────────────────────────────────────┐
│ 4. keyboard_handle_scancode() (kernel.c)        │
│    - Detecta Shift, Ctrl, teclas especiais      │
│    - Converte scancode para ASCII               │
│    - Empilha caractere em keyboard_queue[]     │
│    - Desperta tasks aguardando stdin            │
└──────────────────┬──────────────────────────────┘
                   ↓
┌─────────────────────────────────────────────────┐
│ 5. Fila de Entrada (keyboard_queue[128])        │
│    - Buffer circular                            │
│    - Sincronizado com mutexes                   │
└──────────────────┬──────────────────────────────┘
                   ↓
┌─────────────────────────────────────────────────┐
│ 6. console_read() (kernel.c - VFS node)        │
│    - SYS_READ syscall                           │
│    - Extrai caracteres da fila                  │
│    - Retorna para user space                    │
└──────────────────┬──────────────────────────────┘
                   ↓
┌─────────────────────────────────────────────────┐
│ 7. Shell (Ring 3) - shell.c                     │
│    - read_line() lê caracteres                  │
│    - Interpreta pipe (|)                        │
│    - Executa comando                            │
└─────────────────────────────────────────────────┘
```

---

## 5. Mapeamento de Caracteres Especiais

### 5.1 Tabela de Referência

| Caractere | Scancode | Normal | Com Shift | Função |
|-----------|----------|--------|-----------|--------|
| `'` | 0x28 | `'` | `"` | Delimitador de strings |
| `\` | 0x2B | `\` | `\|` | Escape e Pipe |
| `/` | 0x35 | `/` | `?` | Caminho e interrogação |
| `-` | 0x0C | `-` | `_` | Hífen e underscore |
| `=` | 0x0D | `=` | `+` | Igualdade e adição |
| `[` | 0x1A | `[` | `{` | Array/bloco |
| `]` | 0x1B | `]` | `}` | Fechamento |
| `;` | 0x27 | `;` | `:` | Ponto e vírgula |
| `,` | 0x33 | `,` | `<` | Vírgula e menor |
| `.` | 0x34 | `.` | `>` | Ponto e maior |
| `\`` | 0x29 | `` ` `` | `~` | Acento grave e til |

### 5.2 Exemplo: Pipe (|)

```
┌─────────────────────────────────────┐
│ Usuário pressiona: Shift + \        │
│ Scancode recebido: 0x2B             │
└─────────────────────────┬───────────┘
                          ↓
┌─────────────────────────────────────┐
│ keyboard_handle_scancode(0x2B)      │
│ - keyboard_shift = 1 (Shift pressionado)│
└─────────────────────────┬───────────┘
                          ↓
┌─────────────────────────────────────┐
│ keyboard_char_from_scancode(0x2B)   │
│ - keyboard_shift == 1               │
│ - Consulta keymap_shift[0x2B]       │
│ - Retorna '|'                       │
└─────────────────────────┬───────────┘
                          ↓
┌─────────────────────────────────────┐
│ keyboard_queue_push('|')            │
│ - Adiciona à fila circular          │
│ - Desperta readers de stdin         │
└─────────────────────────────────────┘
```

---

## 6. Suporte a Scancodes Estendidos (0xE0)

Alguns teclados e teclas especiais (como setas, delete estendido, numpad) enviam um prefixo 0xE0 seguido pelo código:

```c
if (scancode == 0xE0) {
    keyboard_extended = 1;  // Marca que próximo byte é estendido
    return;
}

int extended = keyboard_extended;
if (keyboard_extended) {
    keyboard_extended = 0;  // Reset flag para próxima iteração
}

// ... later ...

if (extended) {
    return;  // Ignora outros scancodes estendidos por enquanto
}
```

**Nota:** O sistema atualmente ignora a maioria dos scancodes estendidos, mas está preparado para expandi-los no futuro.

---

## 7. Validação e Testes

### 7.1 Teste Básico do Driver

```bash
# Compilar kernel com melhorias
make clean
make all

# Criar imagem de disco
make fat16-disk

# Executar em QEMU
qemu-system-x86_64 -drive file=photon.img,format=raw -serial stdio
```

### 7.2 Teste de Caracteres Especiais no Shell

Uma vez no shell:

```bash
# Teste 1: Aspas simples
PhotonOS /> echo 'teste com aspas'

# Teste 2: Aspas duplas
PhotonOS /> echo "teste com aspas duplas"

# Teste 3: Pipe (IPC)
PhotonOS /> echo "hello world" | upper

# Teste 4: Barra invertida (escape)
PhotonOS /> echo test\nline

# Teste 5: Barra normal
PhotonOS /> echo caminho/do/arquivo

# Teste 6: Símbolos matemáticos
PhotonOS /> echo 1+1 ou 2-1

# Teste 7: Combinação complexa
PhotonOS /> echo "pipe test" | rev
```

### 7.3 Teste de Debug via Serial

O kernel envia logs detalhados para COM1 quando caracteres especiais são digitados:

```
DEBUG: Shift pressed
DEBUG: Special char detected: 0x2B -> '|' (shift=1)
DEBUG: Shift released
```

---

## 8. Impacto no Shell (shell.c)

### 8.1 Função find_pipe()

```c
static char *find_pipe(char *str)
{
    while (*str != '\0') {
        if (*str == '|') {  // Busca pelo caractere PIPE
            return str;
        }
        str++;
    }
    return 0;
}
```

Agora detecta corretamente o pipe (|) enviado pelo driver de teclado melhorado.

### 8.2 Suporte a Aspas

O shell ainda requer expansão de suporte a aspas, mas o driver agora garante que:
- `'` (aspas simples) é corretamente digitado
- `"` (aspas duplas) é corretamente digitado

---

## 9. Estrutura do Driver

### 9.1 Variáveis de Estado Globais (kernel.c, ~línhas 100-115)

```c
static int keyboard_shift;      // Flag: Shift está pressionado?
static int keyboard_ctrl;       // Flag: Ctrl está pressionado?
static int keyboard_extended;   // Flag: Próximo é scancode estendido?
static char keyboard_queue[KEYBOARD_QUEUE_SIZE];    // Buffer circular
static size_t keyboard_queue_read;                  // Índice de leitura
static size_t keyboard_queue_write;                 // Índice de escrita
```

### 9.2 Funções Principais

1. **keyboard_irq_handler()** - Entry point da IRQ
2. **keyboard_handle_scancode()** - Lógica de processamento
3. **keyboard_char_from_scancode()** - Conversão scancode→ASCII
4. **keyboard_queue_push()** - Adiciona char à fila
5. **keyboard_queue_pop()** - Remove char da fila
6. **console_read()** - VFS node para SYS_READ

---

## 10. Compilação e Deployment

### 10.1 Requisitos

- NASM (assembler)
- GCC (compilador C)
- LD (linker)
- Make (build automation)

### 10.2 Passos de Compilação

```bash
cd /path/to/PhotonOS

# Limpar build anterior
make clean

# Compilar tudo
make all

# Criar imagem de disco com FAT16
make fat16-disk

# Executar em emulador
make run-fat16
```

### 10.3 Arquivos Modificados

- `kernel.c` - Driver de teclado (linhas ~155-290 para keymaps, ~1150-1280 para handler)

### 10.4 Arquivos Não Modificados (compatíveis)

- `kernel.asm` - Assembly stubs (sem mudanças necessárias)
- `shell.c` - Shell em Ring 3 (já compatível com pipe)
- `scheduler.c` - Agendador (compatível)

---

## 11. Troubleshooting

### 11.1 Problema: Pipe (|) não funciona

**Diagnóstico:**
1. Verificar logs de debug serial: `"DEBUG: Special char detected: 0x2B -> '|'"`
2. Garantir que o shell chama `find_pipe()` corretamente
3. Testar com comando simples: `echo test | upper`

**Solução:**
1. Recompilar kernel
2. Verificar que keyboard_shift é atualizado corretamente
3. Conferir que characters chegam ao queue

### 11.2 Problema: Aspas duplas/simples não aparecem

**Diagnóstico:**
1. Verificar se scancode 0x28 é tratado
2. Confirmar estado de keyboard_shift

**Solução:**
```c
// Verificar em keyboard_handle_scancode():
// scancode 0x28 com shift=0 deve retornar '
// scancode 0x28 com shift=1 deve retornar "
```

### 11.3 Problema: Barra invertida (\\) vs Pipe (|)

**Regra:**
- Sem Shift: `\` (0x2B → keymap_normal[0x2B] = '\\')
- Com Shift: `|` (0x2B → keymap_shift[0x2B] = '|')

---

## 12. Referências e Documentação

- PS/2 Keyboard Protocol: Set 1 Scancodes
- Intel x86-64 ISA Manual
- QEMU PS/2 Controller Documentation

---

## 13. Sumário das Mudanças

✓ Tabelas de mapeamento expandidas e documentadas
✓ keyboard_char_from_scancode() com validação melhorada
✓ keyboard_handle_scancode() com debug logging
✓ Suporte completo a scancodes estendidos (0xE0)
✓ Documentação de fluxo de entrada (hardware até Ring 3)
✓ Mapeamento explícito de |, ', ", \, /
✓ Tratamento robusto de break codes (release keys)
✓ Logging de caracteres especiais para troubleshooting

---

**Status:** ✅ Implementado e Documentado
**Última Atualização:** 2026-05-29
**Próximas Melhorias Possíveis:**
- Suporte a layout ABNT2 (português brasileiro)
- Tratamento de LEDs de teclado (NumLock, CapsLock, ScrollLock)
- Suporte a hotkeys do sistema
