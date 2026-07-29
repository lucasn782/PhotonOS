# 🔐 POSIX Simplified Permissions Model in PhotonOS

## Visão Geral

O **PhotonOS** implementa um modelo simplificado de controle de acesso POSIX baseado em proprietário (**UID**), grupo (**GID**) e máscara de permissões Octais (**Mode**).

---

## 🔑 1. Identificação de Usuário e Grupo (UID / GID)

Cada tarefa (`task_control_block`) no escalonador herda e mantém:
- **`uid` (User ID)**: ID numérico do usuário executando a tarefa (`0` representa o Superusuário `root`).
- **`gid` (Group ID)**: ID numérico do grupo primário do usuário.

Tarefas filhas criadas via `fork()` ou `spawn()` herdam os IDs de usuário e grupo da tarefa pai.

---

## 🎭 2. Máscara de Permissões Octais (`mode_t`)

Cada nó no VFS armazena os 9 bits padrão de permissão POSIX (`mode_t` / `0777`):

| Bitmask Octal | Permissão | Significado |
| :---: | :--- | :--- |
| `0700` | `S_IRWXU` | Leitura, escrita e execução do Proprietário (User) |
| `0400` | `S_IRUSR` | Leitura do Proprietário |
| `0200` | `S_IWUSR` | Escrita do Proprietário |
| `0100` | `S_IXUSR` | Execução do Proprietário |
| `0070` | `S_IRWXG` | Leitura, escrita e execução do Grupo (Group) |
| `0007` | `S_IRWXO` | Leitura, escrita e execução de Outros (Other) |

### Permissões Padrão na Criação
- **Diretórios**: `0755` (`rwxr-xr-x`).
- **Arquivos**: `0644` (`rw-r--r--`).

---

## 🛡️ 3. Verificação de Acesso (`vfs_check_permission`)

Antes de realizar operações de leitura, escrita ou execução em um nó do VFS:

```c
int vfs_check_permission(vfs_node_t *node, uint32_t mask, uint32_t uid, uint32_t gid);
```

1. **Superuser (`uid == 0`)**: Acesso total concedido incondicionalmente.
2. **Proprietário (`node->uid == uid`)**: Testado contra os bits de permissão do proprietário `((mode >> 6) & mask) == mask`.
3. **Grupo (`node->gid == gid`)**: Testado contra os bits de grupo `((mode >> 3) & mask) == mask`.
4. **Outros**: Testado contra os bits de outros `(mode & mask) == mask`.

---

## 🛠️ 4. Chamadas de Sistema (`chmod` / `chown`)

- **`chmod(path, mode)`**: Altera os bits de permissão `node->mode` de um arquivo ou diretório.
- **`chown(path, uid, gid)`**: Altera a propriedade de usuário (`uid`) e grupo (`gid`) do nó alvo.
