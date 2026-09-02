#ifndef PHOTONOS_EXT2_H
#define PHOTONOS_EXT2_H

#include <stdint.h>
#include <stddef.h>
#include "vfs.h"

#define EXT2_SUPER_MAGIC 0xEF53

/* EXT2 file types for directory entries */
#define EXT2_FT_UNKNOWN  0
#define EXT2_FT_REG_FILE 1
#define EXT2_FT_DIR      2
#define EXT2_FT_CHRDEV   3
#define EXT2_FT_BLKDEV   4
#define EXT2_FT_FIFO     5
#define EXT2_FT_SOCK     6
#define EXT2_FT_SYMLINK  7

struct ext2_superblock {
    uint32_t s_inodes_count;       /* Inodes count */
    uint32_t s_blocks_count;       /* Blocks count */
    uint32_t s_r_blocks_count;     /* Reserved blocks count */
    uint32_t s_free_blocks_count;  /* Free blocks count */
    uint32_t s_free_inodes_count;  /* Free inodes count */
    uint32_t s_first_data_block;   /* First Data Block */
    uint32_t s_log_block_size;     /* Block size */
    uint32_t s_log_frag_size;      /* Fragment size */
    uint32_t s_blocks_per_group;   /* # Blocks per group */
    uint32_t s_frags_per_group;    /* # Fragments per group */
    uint32_t s_inodes_per_group;   /* # Inodes per group */
    uint32_t s_mtime;              /* Mount time */
    uint32_t s_wtime;              /* Write time */
    uint16_t s_mnt_count;          /* Mount count */
    uint16_t s_max_mnt_count;      /* Maximal mount count */
    uint16_t s_magic;              /* Magic signature (0xEF53) */
    uint16_t s_state;              /* File system state */
    uint16_t s_errors;             /* Behaviour when detecting errors */
    uint16_t s_minor_rev_level;    /* minor revision level */
    uint32_t s_lastcheck;          /* time of last check */
    uint32_t s_checkinterval;      /* max. time between checks */
    uint32_t s_creator_os;         /* OS */
    uint32_t s_rev_level;          /* Revision level */
    uint16_t s_def_resuid;         /* Default uid for reserved blocks */
    uint16_t s_def_resgid;         /* Default gid for reserved blocks */
    /* -- EXT2_DYNAMIC_REV Specific -- */
    uint32_t s_first_ino;          /* First non-reserved inode */
    uint16_t s_inode_size;         /* size of inode structure */
    uint16_t s_block_group_nr;     /* block group # of this superblock */
    uint32_t s_feature_compat;     /* compatible feature set */
    uint32_t s_feature_incompat;   /* incompatible feature set */
    uint32_t s_feature_ro_compat;  /* readonly-compatible feature set */
    uint8_t  s_uuid[16];           /* 128-bit uuid for volume */
    char     s_volume_name[16];    /* volume name */
    char     s_last_mounted[64];   /* directory where last mounted */
    uint32_t s_algorithm_usage_bitmap; /* For compression */
    /* -- Performance Hints -- */
    uint8_t  s_prealloc_blocks;    /* Nr of blocks to try to preallocate*/
    uint8_t  s_prealloc_dir_blocks;/* Nr to preallocate for dirs */
    uint16_t s_padding1;
    /* -- Journaling Support -- */
    uint8_t  s_journal_uuid[16];   /* uuid of journal superblock */
    uint32_t s_journal_inum;       /* inode number of journal file */
    uint32_t s_journal_dev;        /* device number of journal file */
    uint32_t s_last_orphan;        /* start of list of inodes to delete */
    uint32_t s_hash_seed[4];       /* HTREE hash seed */
    uint8_t  s_def_hash_version;   /* Default hash version to use */
    uint8_t  s_reserved_char_pad;
    uint16_t s_reserved_word_pad;
    uint32_t s_default_mount_opts;
    uint32_t s_first_meta_bg;      /* First metablock block group */
    uint32_t s_reserved[190];      /* Padding to 1024 bytes */
} __attribute__((packed));

struct ext2_group_desc {
    uint32_t bg_block_bitmap;      /* Blocks bitmap block */
    uint32_t bg_inode_bitmap;      /* Inodes bitmap block */
    uint32_t bg_inode_table;       /* Inodes table block */
    uint16_t bg_free_blocks_count; /* Free blocks count */
    uint16_t bg_free_inodes_count; /* Free inodes count */
    uint16_t bg_used_dirs_count;   /* Directories count */
    uint16_t bg_pad;
    uint32_t bg_reserved[3];
} __attribute__((packed));

struct ext2_inode {
    uint16_t i_mode;        /* File mode */
    uint16_t i_uid;         /* Low 16 bits of Owner Uid */
    uint32_t i_size;        /* Size in bytes */
    uint32_t i_atime;       /* Access time */
    uint32_t i_ctime;       /* Creation time */
    uint32_t i_mtime;       /* Modification time */
    uint32_t i_dtime;       /* Deletion Time */
    uint16_t i_gid;         /* Low 16 bits of Group Id */
    uint16_t i_links_count; /* Links count */
    uint32_t i_blocks;      /* Blocks count in 512-byte sectors */
    uint32_t i_flags;       /* File flags */
    uint32_t i_osd1;        /* OS dependent 1 */
    uint32_t i_block[15];   /* Pointers to blocks: 12 direct, 1 singly-indirect, 1 doubly-indirect, 1 triply-indirect */
    uint32_t i_generation;  /* File version (for NFS) */
    uint32_t i_file_acl;    /* File ACL */
    uint32_t i_dir_acl;     /* Directory ACL */
    uint32_t i_faddr;       /* Fragment address */
    uint8_t  i_osd2[12];    /* OS dependent 2 */
} __attribute__((packed));

struct ext2_dir_entry_2 {
    uint32_t inode;         /* Inode number */
    uint16_t rec_len;       /* Directory entry length */
    uint8_t  name_len;      /* Name length */
    uint8_t  file_type;     /* File type */
    char     name[255];     /* File name */
} __attribute__((packed));

struct ext2_node_data {
    uint32_t inode_num;
};

int ext2_mount(uint32_t partition_lba);
int ext2_mount_at(vfs_node_t *mount_point, uint32_t partition_lba);
int ext2_read_inode(uint32_t inode_num, struct ext2_inode *out_inode);
int ext2_write_inode(uint32_t inode_num, struct ext2_inode *inode);
uint32_t ext2_alloc_inode(void);
uint32_t ext2_alloc_block(void);
int ext2_vfs_create(const char *path);

#endif
