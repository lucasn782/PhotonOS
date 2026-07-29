# 📁 Virtual File System (VFS) Architecture in PhotonOS

## Visão Geral

O **Virtual File System (VFS)** do **PhotonOS** é a camada de abstração do kernel responsável por fornecer uma interface uniforme e thread-safe para manipulação de arquivos, diretórios, dispositivos físicos (`/dev`), conexões de rede (`socket`), pipes e links em múltiplos sistemas de arquivos subjacentes (como **FAT16** e **EXT2**).

---

## 🏗️ 1. Estrutura de Nós (`vfs_node_t`)

Cada elemento na árvore hierárquica do VFS é representado pela estrutura `vfs_node_t`:

```c
typedef enum vfs_node_type {
    VFS_NODE_FILE,
    VFS_NODE_DIRECTORY,
    VFS_NODE_DEVICE,
    VFS_NODE_PIPE,
    VFS_NODE_SOCKET,
    VFS_NODE_SYMLINK,
} vfs_node_type_t;

struct vfs_node {
    char name[VFS_NAME_MAX];
    vfs_node_type_t type;
    size_t size;
    uint32_t uid;
    uint32_t gid;
    uint32_t mode;
    uint32_t nlink;
    char symlink_target[VFS_NAME_MAX];
    void *data;
    vfs_read_t read;
    vfs_write_t write;
    vfs_open_t open;
    vfs_close_t close;
    vfs_readdir_t readdir;
    vfs_node_t *parent;
    vfs_node_t *child;
    vfs_node_t *sibling;
    vfs_node_t *mounted_here;
};
```

---

## 🔗 2. Hard Links & Symbolic Links (Symlinks)

### Hard Links (`link()`, `unlink()`)
- **Criação (`vfs_link`)**: Associa um novo nome/nó no VFS a um bloco de dados existente, incrementando o contador de referências físicas `nlink` do nó de origem (`old_node->nlink++`).
- **Remoção (`vfs_unlink`)**: Decrementa `nlink`. O nó e seus blocos físicos correspondentes só são desalocados da memória quando `nlink == 0`.

### Symbolic Links (`symlink()`, `readlink()`)
- **Tipo de Nó**: `VFS_NODE_SYMLINK`.
- **Conteúdo**: O atributo `symlink_target` armazena o caminho de destino relativo ou absoluto.
- **Resolução de Caminho**: A função `vfs_find_following_symlinks(path, max_depth)` resolve recursivamente symlinks até o limite de segurança `max_depth = 8` para evitar loops infinitos (`ELOOP`).

---

## 🛠️ 3. Operações de Chamada de Sistema

| Syscall | Parâmetros | Descrição |
| :--- | :--- | :--- |
| `sys_link` | `const char *oldpath, const char *newpath` | Cria um hard link para um arquivo existente. |
| `sys_unlink` | `const char *pathname` | Remove uma entrada de diretório e decrementa `nlink`. |
| `sys_symlink` | `const char *target, const char *linkpath` | Cria um link simbólico apontando para `target`. |
| `sys_readlink` | `const char *path, char *buf, size_t bufsiz` | Lê o caminho alvo armazenado em um link simbólico. |
