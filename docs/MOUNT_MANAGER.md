# 🗺️ Mount Manager & Multi-Volume Table in PhotonOS

## Visão Geral

O **Mount Manager** do PhotonOS gerencia a montagem dinâmica e transparente de múltiplos volumes e sistemas de arquivos em pontos de montagem arbitrários do **Virtual File System (VFS)**.

---

## 📋 1. Tabela Global de Mounts (`vfs_mount_list`)

Todos os pontos de montagem ativos são mantidos na lista encadeada `vfs_mount_list` encabeçada no kernel:

```c
typedef struct vfs_mount {
    char mount_point[VFS_NAME_MAX]; /* e.g. "/", "/mnt/ext2" */
    char source[VFS_NAME_MAX];      /* e.g. "/dev/hda", "none" */
    char fs_type[32];               /* e.g. "fat16", "ext2" */
    vfs_node_t *root_node;          /* Nó raiz do sistema de arquivos montado */
    vfs_node_t *mount_over;         /* Nó original sobre o qual ocorreu a montagem */
    struct vfs_mount *next;
} vfs_mount_t;

extern vfs_mount_t *vfs_mount_list;
```

---

## 🔀 2. Redirecionamento Transparente no VFS (`vfs_find`)

Durante a navegação por caminhos de diretório na função `vfs_find()`:
1. Para cada nó pesquisado `current`, o kernel inspeciona o ponteiro `current->mounted_here`.
2. Se `mounted_here != NULL`, a busca intercepta a travessia e pula transparente para o nó raiz do sistema de arquivos montado naquele ponto (`current->mounted_here`).
3. Isso permite que um único caminho hierárquico (ex: `/mnt/ext2/arquivo.txt`) navegue continuamente do filesystem raiz FAT16 para o volume secundário EXT2.

---

## 🔧 3. Operações de Montagem (`mount` / `umount`)

### Montar um Volume (`vfs_mount`)
```c
int vfs_mount(const char *source, const char *target, const char *fs_type, uint64_t flags);
```
- Localiza o nó de destino no VFS.
- Inicializa a raiz do novo sistema de arquivos.
- Vincula `target_node->mounted_here = fs_root`.
- Insere a entrada na tabela global `vfs_mount_list`.

### Desmontar um Volume (`vfs_umount`)
```c
int vfs_umount(const char *target);
```
- Localiza a entrada correspondente em `vfs_mount_list`.
- Desconecta o redirecionamento `mount_over->mounted_here = NULL`.
- Desaloca a estrutura de montagem de forma segura.
