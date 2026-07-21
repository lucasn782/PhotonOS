# 📦 Especificação Técnica — Sistema de Ficheiros EXT2 Nativo Gravável

## PhotonOS v4.0 — Trilha 10: Subsistema de Armazenamento Persistente EXT2

---

## 1. Visão Geral da Arquitetura

O PhotonOS v4.0 implementa suporte nativo gravável de alta performance ao sistema de ficheiros **Second Extended Filesystem (EXT2)** no kernel Ring 0, operando em modo PIO (*Programmed I/O*) sobre o barramento IDE primário. O subsistema é composto por quatro camadas verticais de engenharia:

```text
┌─────────────────────────────────────────────────────────┐
│           Espaço de Usuário (Ring 3)                    │
│     shell: touch, write, cat, ls, > (redirecionamento)  │
├─────────────────────────────────────────────────────────┤
│           Virtual File System (VFS)                     │
│     vfs_read() · vfs_write() · vfs_readdir()            │
│     vfs_find() · vfs_create_node()                      │
├─────────────────────────────────────────────────────────┤
│           Driver EXT2 (src/fs/ext2.c)                   │
│     ext2_mount() · ext2_read_inode() · ext2_write()     │
│     ext2_alloc_block() · ext2_alloc_inode()             │
│     ext2_add_dir_entry() · ext2_vfs_create()            │
├─────────────────────────────────────────────────────────┤
│           Driver ATA/IDE (src/drivers/ata.c)            │
│     ata_read_sectors() · ata_write_sectors()            │
│     ata_mutex · Portas 0x1F0–0x1F7 · CMD 0x20/0x30     │
├─────────────────────────────────────────────────────────┤
│           Hardware: Controlador IDE Primário             │
│     Registradores de Comando/Status/Dados/LBA           │
└─────────────────────────────────────────────────────────┘
```

### 1.1 Marco Histórico de Persistência — PhotonOS v4.0
A conclusão da Trilha 10 introduziu um subsistema de armazenamento persistente capaz de operar como uma camada de I/O block-device de baixa latência no kernel. O fluxo de escrita passou a ser coordenado em três níveis: blindagem concorrente no driver ATA, resolução de metadados EXT2 em memória e tradução de pedidos VFS para blocos físicos do disco. A implementação cobre desde a leitura do superbloco até à criação de entradas de diretório e à atribuição de blocos de dados, tudo com semântica de estado persistente e sem dependência de um sistema de ficheiros de usuário.

### Ficheiros-Fonte do Subsistema

| Ficheiro | Função |
| :--- | :--- |
| `src/drivers/ata.c` | Driver de disco IDE/ATA com mutex de concorrência SMP |
| `src/fs/ext2.c` | Parser EXT2, operações de inodes, alocação de blocos e escrita |
| `include/fs/ext2.h` | Estruturas on-disk EXT2 e declarações de API |
| `include/ata.h` | Interface pública do driver ATA |
| `include/vfs.h` | Abstração VFS com ponteiros de função (`read`, `write`, `readdir`) |

---

## 2. Blindagem Concorrente no Driver IDE/ATA

### 2.1 Problema: Race Conditions em SMP

O PhotonOS opera em **Multiprocessamento Simétrico (SMP)** com múltiplos núcleos x86_64 ativos simultaneamente. O controlador IDE primário expõe um único conjunto de registradores de I/O mapeados em portas fixas:

| Porta | Registrador | Uso |
| :---: | :--- | :--- |
| `0x1F0` | Data Register | Transferência de dados (16-bit, via `insw`/`outsw`) |
| `0x1F2` | Sector Count | Número de setores a transferir |
| `0x1F3` | LBA Low | Bits [7:0] do endereço LBA28 |
| `0x1F4` | LBA Mid | Bits [15:8] do endereço LBA28 |
| `0x1F5` | LBA High | Bits [23:16] do endereço LBA28 |
| `0x1F6` | Drive/Head | Seleção de drive + bits [27:24] do LBA |
| `0x1F7` | Command/Status | Emissão de comando (escrita) / Leitura de status |
| `0x3F6` | Control | Registrador de controle alternativo |

Sem serialização, dois núcleos podem intercalar escritas nos registradores de LBA e comando, causando corrupção silenciosa de dados no disco.

### 2.2 Solução: `ata_mutex`

O driver ATA introduz uma instância estática de `mutex_t ata_mutex` (ficheiro `src/drivers/ata.c`, linha 32), inicializada em `ata_init()` via `mutex_init(&ata_mutex)`. Este mecanismo protege o acesso direto aos registradores físicos de comando e controlo do disco expostos nas portas `0x1F0`–`0x1F7`, impedindo que múltiplos núcleos simétricos (SMP) intercalem escritas de LBA, contagem de setores, seleção de drive e emissão de comandos ATA. Em termos de engenharia de baixo nível, a sequência `setup de registradores → comando `0x20`/`0x30` → leitura/escrita por `insw`/`outsw` → flush de cache `0xE7`` é tratada como uma seção crítica indivisível, eliminando race conditions de estado e corrupção de pipeline sobre o controlador IDE primário.

**Protocolo de aquisição:**

```text
ata_read_sectors() / ata_write_sectors()
│
├── Validação de parâmetros (sem lock)
├── mutex_lock(&ata_mutex)          ← Seção crítica: início
│   ├── ata_wait_ready()
│   ├── outb(ATA_DRIVE, ...)
│   ├── outb(ATA_SECTOR_COUNT, ...)
│   ├── outb(ATA_LBA_LOW/MID/HIGH, ...)
│   ├── outb(ATA_COMMAND, 0x20|0x30)
│   ├── loop: insw()/outsw() por setor
│   └── [write only] outb(ATA_CMD_CACHE_FLUSH)
├── mutex_unlock(&ata_mutex)        ← Seção crítica: fim
└── Retorno do resultado
```

**Invariante de segurança:** A sequência atômica *setup de registradores → emissão de comando → transferência de dados* é indivisível. Nenhum outro núcleo pode emitir um comando ATA enquanto uma transferência estiver ativa. Em caso de falha durante a seção crítica (timeout de DRQ ou erro de BSY), o mutex é liberado incondicionalmente antes do retorno de erro.

### 2.3 Cache Flush Síncrono

Após o loop `outsw` de escrita, o driver emite o comando `ATA_CMD_CACHE_FLUSH` (`0xE7`) para forçar a persistência dos dados no meio físico, aguardando a confirmação via `ata_wait_ready()` antes de liberar o mutex. Isto garante durabilidade (*write-through*) em nível de setor.

---

## 3. Parser do Superbloco e Tabela de Descritores de Grupo

### 3.1 Leitura e Validação do Superbloco

O Superbloco EXT2 reside num offset fixo de **1024 bytes** a partir do início da partição. Como cada setor ATA possui 512 bytes, o Superbloco ocupa os setores **LBA+2** e **LBA+3** relativos ao início da partição.

A função `ext2_mount()` executa o seguinte pipeline de validação, com foco em robustez estrutural e detecção precoce de volumes não suportados:

1. **Leitura via ATA:** `ata_read_sectors(partition_lba + 2, 2, sb_buf)` — carrega 1024 bytes (2 setores) num buffer temporário alocado por `kmalloc`.
2. **Cópia estrutural:** `memory_copy(&sb, sb_buf, sizeof(struct ext2_superblock))` — popula a variável estática global `sb` com os 1024 bytes do Superbloco.
3. **Validação do Número Mágico:** `sb.s_magic != EXT2_SUPER_MAGIC` — verifica o campo `s_magic` (offset 56 bytes na struct) contra a constante `0xEF53`. Se a assinatura não coincidir, a montagem é abortada imediatamente.
4. **Derivação do Tamanho de Bloco:** `block_size = 1024 << sb.s_log_block_size` — o campo `s_log_block_size` armazena o expoente logarítmico base-2 relativo a 1024. Valores típicos: `0` → 1 KiB, `1` → 2 KiB, `2` → 4 KiB.
5. **Cálculo do Número de Grupos:** `num_groups = ⌈s_blocks_count / s_blocks_per_group⌉`.

### 3.2 Tabela de Descritores de Grupos de Blocos (BGDT)

A BGDT é um array contíguo de `struct ext2_group_desc` (32 bytes cada) armazenado no bloco imediatamente posterior ao Superbloco (`s_first_data_block + 1`). Cada descritor contém:

| Campo | Tamanho | Função |
| :--- | :---: | :--- |
| `bg_block_bitmap` | 4 bytes | Bloco que contém o bitmap de alocação de blocos de dados |
| `bg_inode_bitmap` | 4 bytes | Bloco que contém o bitmap de alocação de inodes |
| `bg_inode_table` | 4 bytes | Bloco inicial da tabela de inodes do grupo |
| `bg_free_blocks_count` | 2 bytes | Contador de blocos livres (atualizado em alocação) |
| `bg_free_inodes_count` | 2 bytes | Contador de inodes livres (atualizado em alocação) |
| `bg_used_dirs_count` | 2 bytes | Número de diretórios no grupo |

O driver carrega a BGDT inteira em RAM via `kmalloc` durante `ext2_mount()`, mantendo o ponteiro global `bg_desc` para acesso O(1) a qualquer grupo durante operações subsequentes. Este mapeamento em memória permite localizar dinamicamente os bitmaps de blocos e inodes, os blocos da tabela de inodes e os contadores de recursos livres de cada grupo, convertendo uma referência lógica de grupo numa estrutura de metadados pronta para uso pelo allocator e pelas rotinas de lookup.

---

## 4. Matemática de Inodes e Resolução de Caminhos

### 4.1 Conversão de Índice Lógico para Offset Físico

A localização de um inode no disco é determinada por aritmética modular de inteiros sobre os parâmetros do Superbloco.

**Funções:** `ext2_read_inode()` e `ext2_write_inode()` (ficheiro `src/fs/ext2.c`, linhas 103–161).

**Algoritmo de resolução para o inode N:**

```text
group          = (N - 1) / s_inodes_per_group
index_in_group = (N - 1) % s_inodes_per_group
inode_size     = (s_rev_level == 0) ? 128 : s_inode_size

inode_table_block = bg_desc[group].bg_inode_table
target_block      = inode_table_block + (index_in_group × inode_size) / block_size
offset_in_block   = (index_in_group × inode_size) % block_size
```

O driver lê o bloco `target_block` inteiro da Tabela de Inodes via ATA, extrai 128 bytes a partir de `offset_in_block`, e popula a `struct ext2_inode` de saída. O processo inverso (`ext2_write_inode`) executa uma operação **Read-Modify-Write**: lê o bloco, sobrescreve os bytes do inode na posição calculada, e grava o bloco completo de volta ao disco. A aritmética de localização é determinística: `group = (N - 1) / s_inodes_per_group`, `index = (N - 1) % s_inodes_per_group`, seguida de cálculo de `block` e `offset_in_block`, permitindo a resolução de qualquer inode do volume sem varredura sequencial do disco.

### 4.2 Inode Raiz e Navegação Recursiva de Diretórios

O EXT2 define o **Inode 2** como a raiz do sistema de ficheiros. A função `ext2_mount_dir()` implementa a navegação recursiva do namespace e integra o lookup do VFS com a representação on-disk do diretório. Cada bloco de dados de um diretório é tratado como um array dinâmico de `struct ext2_dir_entry_2`, no qual `rec_len` governa a progressão entre entradas e permite inspeção incremental de nomes, tipos e inodes sem depender de uma tabela centralizada.

1. Lê o inode do diretório via `ext2_read_inode()`.
2. Percorre cada bloco de dados do diretório interpretando-os como um array dinâmico de `struct ext2_dir_entry_2`:
   - Cada entrada contém: `inode` (4B), `rec_len` (2B), `name_len` (1B), `file_type` (1B), `name[]` (variável).
   - O campo `rec_len` indica o salto até a próxima entrada (permite padding e reutilização de espaço).
3. Para cada entrada válida (excluindo `.` e `..`):
   - Cria um `vfs_node_t` via `vfs_create_node()`.
   - Associa `struct ext2_node_data` com o número do inode para posterior resolução.
   - Instala os ponteiros de função VFS: `read`, `write`, `open`, `readdir`.
   - Se for diretório (`file_type == 2`), invoca `ext2_mount_dir()` recursivamente.

### 4.3 Integração com VFS

O VFS do PhotonOS utiliza um modelo de ponteiros de função na `struct vfs_node`:

| Ponteiro VFS | Implementação EXT2 | Assinatura |
| :--- | :--- | :--- |
| `read` | `ext2_vfs_read` | `int (node, offset, size, buffer)` |
| `write` | `ext2_vfs_write` | `size_t (node, offset, size, buffer)` |
| `open` | `ext2_vfs_open` | `int (node)` |
| `readdir` | `ext2_vfs_readdir` | `int (node, index, entry)` |

A montagem configura transparentemente o nó VFS raiz (`vfs_root()`) com os callbacks EXT2, permitindo que chamadas `vfs_read()`, `vfs_write()` e `vfs_readdir()` despachem automaticamente para o driver EXT2 sem qualquer conhecimento do sistema de ficheiros subjacente pelo código cliente.

---

## 5. Pipeline de Escrita e Alocação Dinâmica

### 5.1 Motor de Varredura Atômica de Bitmaps

O EXT2 mantém dois bitmaps por grupo de blocos: um para **blocos de dados** e outro para **inodes**. Cada bit `0` indica um recurso livre; `1` indica recurso alocado. O motor de varredura atômica percorre o bitmap byte a byte, fixa o primeiro bit livre e atualiza o contador `bg_free_blocks_count` ou `bg_free_inodes_count` antes de persistir o bitmap e os metadados de grupo no dispositivo, garantindo retenção de recursos livres sem regressão de consistência em ambientes SMP.

**`ext2_alloc_block()`** — Alocação de bloco de dados:
1. `mutex_lock(&ext2_mutex)` — exclusão mútua para proteger os bitmaps contra race conditions SMP.
2. Itera sobre todos os grupos de blocos procurando `bg_free_blocks_count > 0`.
3. Lê o bitmap de blocos (`bg_block_bitmap`) via `ext2_read_block()`.
4. Invoca `find_free_bit()`: varredura bit-a-bit para localizar o primeiro bit `0`.
5. Seta o bit correspondente (`bitmap[byte] |= (1 << bit)`).
6. Grava o bitmap atualizado no disco via `ext2_write_block()`.
7. Decrementa `bg_desc[g].bg_free_blocks_count` e `sb.s_free_blocks_count`.
8. Persiste os descritores de grupo e o Superbloco no disco (sincronização imediata).
9. `mutex_unlock(&ext2_mutex)` — libera a seção crítica.
10. Retorna o número absoluto do bloco: `grupo × s_blocks_per_group + free_bit + s_first_data_block`.

**`ext2_alloc_inode()`** — Procedimento idêntico, operando sobre `bg_inode_bitmap` e `bg_free_inodes_count`. O número do inode retornado é: `grupo × s_inodes_per_group + free_bit + 1`.

### 5.2 Suporte a Ponteiros Diretos e Simplesmente Indiretos

O campo `i_block[15]` de cada inode EXT2 organiza o mapeamento de blocos lógicos para blocos físicos:

```text
i_block[0..11]   → 12 ponteiros diretos (até 12 × block_size bytes)
i_block[12]      → 1 ponteiro simplesmente indireto
                    (aponta para um bloco contendo block_size/4 ponteiros)
i_block[13]      → 1 ponteiro duplamente indireto (não implementado)
i_block[14]      → 1 ponteiro triplamente indireto (não implementado)
```

A função `ext2_get_phys_block(inode, file_block_num)` resolve o mapeamento:
- Se `file_block_num < 12`: retorna `inode->i_block[file_block_num]` diretamente.
- Se `12 ≤ file_block_num < 12 + entries_per_block`: lê o bloco apontado por `i_block[12]`, indexa o array de `uint32_t` na posição `file_block_num - 12`.

A função inversa `ext2_set_phys_block()` vincula novos blocos ao inode, alocando automaticamente o bloco indireto (`i_block[12]`) se necessário pela primeira vez, inicializando-o com zeros.

### 5.3 Função `ext2_vfs_write()`

O pipeline de escrita completo:

```text
ext2_vfs_write(node, offset, size, buffer)
│
├── Lê o inode corrente do disco
├── Calcula blocos necessários: ⌈(offset + size) / block_size⌉
├── Se required_blocks > current_blocks:
│   └── Loop: ext2_alloc_block() + ext2_set_phys_block()
│       para cada bloco faltante
├── Loop de escrita por chunk:
│   ├── Resolve bloco físico via ext2_get_phys_block()
│   ├── Se escrita parcial (offset dentro do bloco):
│   │   └── Read-Modify-Write do bloco
│   ├── memory_copy(block_buf + offset, user_buffer, chunk)
│   └── ext2_write_block(phys_block, block_buf)
├── Atualiza i_size se (offset + written) > i_size
├── Persiste o inode no disco via ext2_write_inode()
└── Retorna bytes_written
```

### 5.4 Algoritmo de Divisão de Entradas de Diretório

A função `ext2_add_dir_entry()` insere novas entradas em diretórios EXT2 utilizando o algoritmo de **divisão de `rec_len`** (*directory entry splitting*): o kernel calcula `needed_len = 8 + ((name_len + 3) & ~3)`, procura uma entrada livre ou espaço residual numa entrada existente e, se necessário, aloca um bloco de diretório adicional para acomodar o novo inode. O procedimento preserva o layout on-disk do diretório, evita fragmentação estrutural e mantém o namespace consistente durante a criação de novos nós.

**Cálculo do espaço necessário:**
```text
needed_len = 8 + ((name_len + 3) & ~3)
```
Onde `8` = cabeçalho fixo (inode + rec_len + name_len + file_type) e o nome é alinhado a 4 bytes.

**Estratégia de inserção (por ordem de prioridade):**

1. **Reutilização de entrada deletada:** Se `entry->inode == 0` e `entry->rec_len >= needed_len`, preenche a entrada in-place.
2. **Divisão de trailing space:** Se uma entrada existente tem `rec_len >= min_rec_len + needed_len`, o `rec_len` original é reduzido para `min_rec_len` e uma nova entrada é criada no espaço residual com `rec_len = old_rec_len - min_rec_len`.
3. **Alocação de novo bloco:** Se nenhum espaço existente for suficiente, `ext2_alloc_block()` aloca um novo bloco de diretório, cria uma única entrada ocupando todo o bloco (`rec_len = block_size`), e atualiza `i_size` e `i_blocks` do inode do diretório.

### 5.5 Criação de Ficheiros (`ext2_vfs_create`)

O pipeline completo de criação de um novo ficheiro no disco:

1. `ext2_alloc_inode()` — reserva um inode livre via bitmap atômico.
2. Inicializa o inode com `i_mode = 0x81A4` (ficheiro regular, `rw-r--r--`), `i_links_count = 1`.
3. `ext2_write_inode()` — persiste o inode no disco.
4. `ext2_add_dir_entry(2, name, inode_num, EXT2_FT_REG_FILE)` — insere a entrada no diretório raiz (Inode 2).
5. `vfs_create_node()` — cria o nó VFS correspondente na árvore em memória.
6. Instala os callbacks VFS (`read`, `write`, `open`, `readdir`).

---

## 6. Regras de Ring 0 e Conformidade

### 6.1 Restrição Estrita de `klog`

Todas as chamadas a `klog()` no driver EXT2 utilizam exclusivamente strings literais estáticas sem especificadores de formato:

```c
klog("EXT2: Sistema de ficheiros montado com sucesso.\n");
klog("EXT2: Assinatura magica invalida.\n");
klog("EXT2: Falha ao ler o superbloco.\n");
```

Nenhuma chamada contém `%d`, `%x`, `%s` ou qualquer outro especificador — em conformidade com a regra de Ring 0 do PhotonOS.

### 6.2 Thread Safety Primitiva

| Mutex | Ficheiro | Protege |
| :--- | :--- | :--- |
| `ata_mutex` | `src/drivers/ata.c` | Registradores de I/O `0x1F0`–`0x1F7` durante leitura/escrita de setores |
| `ext2_mutex` | `src/fs/ext2.c` | Bitmaps de blocos e inodes durante alocação atômica |
| `vfs_mutex` | `src/kernel/vfs.c` | Árvore VFS durante criação de nós e traversal de caminhos |

### 6.3 Fallback FAT16 → EXT2

O driver ATA implementa detecção automática de sistema de ficheiros em `ata_vfs_init()`:

```c
if (!fat16_mount(0)) {
    (void)ext2_mount(0);
}
```

Se o disco contiver uma partição FAT16 válida (com assinatura BPB `0x55AA`), o FAT16 é montado. Caso contrário, o driver tenta montar como EXT2 (validando `0xEF53`). O mesmo padrão de fallback aplica-se a `ata_vfs_create()`.

---

## 7. Estruturas de Dados On-Disk (Referência)

### `struct ext2_superblock` (1024 bytes, `include/fs/ext2.h`)

Campos críticos para o driver:
- `s_inodes_count`, `s_blocks_count` — contadores globais
- `s_free_blocks_count`, `s_free_inodes_count` — atualizados atomicamente em alocação
- `s_log_block_size` — expoente para cálculo do tamanho de bloco
- `s_blocks_per_group`, `s_inodes_per_group` — usados em aritmética de localização
- `s_magic` — `0xEF53` (validação obrigatória)
- `s_inode_size` — tamanho do inode (128 para rev 0, configurável para rev 1+)
- `s_first_data_block` — offset do primeiro bloco de dados (geralmente 0 ou 1)

### `struct ext2_inode` (128 bytes)

- `i_mode` — tipo de ficheiro e permissões (ex: `0x81A4` = regular `rw-r--r--`)
- `i_size` — tamanho do ficheiro em bytes
- `i_blocks` — número de setores de 512 bytes alocados
- `i_block[15]` — array de mapeamento de blocos (12 diretos + 3 indiretos)

### `struct ext2_dir_entry_2` (variável)

- `inode` — número do inode referenciado
- `rec_len` — comprimento total da entrada (incluindo padding)
- `name_len` — comprimento do nome do ficheiro
- `file_type` — tipo do ficheiro (1=regular, 2=diretório, 7=symlink)
- `name[255]` — nome do ficheiro (sem terminador null no disco)

---

*Última atualização: v4.0 — PhotonOS Writable EXT2 Filesystem*
