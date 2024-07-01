#include "wfs.h"

#include <errno.h>
#include <fuse.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define DIRENTS (BLOCK_SIZE / sizeof(struct wfs_dentry))

char *data_map;
struct wfs_sb sb;
char *inode_bmap;
char *block_bmap;

void split(char *path, char **child) {
  *child = NULL;
  for (int i = strlen(path) - 1; i >= 0; i--) {
    if (path[i] == '/') {
      path[i] = '\0';
      *child = &path[i+1];

      break;
    }
  }
}

void free_dblock(off_t y) {
  off_t x = (y - sb.d_blocks_ptr) / BLOCK_SIZE;
  off_t ind = x / 8;
  off_t bitnum = x % 8;
  uint8_t mask = 1 << bitnum;
  block_bmap[ind] ^= mask;
}

int get_inode(const char *path, struct wfs_inode **ret) {
  *ret = NULL;

  char *path_cpy = strdup(path);
  off_t next_inode = 0;
  struct wfs_dentry *ents;

  struct wfs_inode *cur = (struct wfs_inode*)(data_map + sb.i_blocks_ptr);
  if (cur == NULL) {
    return -1;
  }
  char *tok = strtok(path_cpy, "/");

  while (tok != NULL) {
    next_inode = 0;
    if (S_ISDIR(cur->mode)) {
      for (int i = 0; i < cur->nlinks; i++) {
        ents = (struct wfs_dentry *)&data_map[cur->blocks[i / DIRENTS]];
        struct wfs_dentry *ent = &ents[i % DIRENTS];

        if (!strcmp(ent->name, tok)) {
          next_inode = ent->num;
          break;
        }
      }

      if (next_inode == 0) {
        return -1;
      }
    } else {
      return -1;
    }
    cur = (struct wfs_inode *)(data_map + sb.i_blocks_ptr + next_inode * BLOCK_SIZE);
    tok = strtok(NULL, "/");
  }

  free(path_cpy);

  *ret = cur;

  return 0;
}

int alloc_inode() {
  char *ptr = (char *)(data_map + sb.i_bitmap_ptr);
  for (int j = 0; j < sb.num_inodes / 8; j++) {
    for (int i = 0; i < 8; i++) {
      int bitnum = 1 << i;
      int bit = bitnum & ptr[j];
      if (!bit) {
        ptr[j] |= bitnum;
        return j * 8 + i;
      }
    }
  }

  return 0;
}

int alloc_dblock() {
  unsigned char *ptr = (unsigned char *)(data_map + sb.d_bitmap_ptr);
  for (int j = 0; j < sb.num_data_blocks / 8; j++) {
    for (int i = 0; i < 8; i++) {
      int bitnum = 1 << i;
      int bit = bitnum & ptr[j];
      if (!bit) {
        ptr[j] |= bitnum;
        return (j * 8 + i) * BLOCK_SIZE + sb.d_blocks_ptr;
      }
    }
  }

  return 0;
}

int wfs_getattr(const char *path, struct stat *stbuf) {
  struct wfs_inode *node;
  if (get_inode(path, &node) < 0) {
      errno = ENOENT;
      return -ENOENT;
    }

  stbuf->st_uid = node->uid;
  stbuf->st_gid = node->gid;
  stbuf->st_atim.tv_sec = node->atim;
  stbuf->st_mtim.tv_sec = node->mtim;
  stbuf->st_ctim.tv_sec = node->ctim;
  stbuf->st_mode = node->mode;
  stbuf->st_size = node->size;
  stbuf->st_nlink = node->nlinks;
  stbuf->st_ino = node->num;

  return 0;
}

int add_entry(const char *path, mode_t mode) {
  char *path_cpy = strdup(path);

  char *child_str;
  split(path_cpy, &child_str);

  struct wfs_inode *parent_node;
  get_inode(path_cpy, &parent_node);

  if (parent_node->nlinks >= (N_BLOCKS - 1) * DIRENTS) {
      errno = ENOSPC;
      return -ENOSPC;
    }

  if (parent_node->nlinks % DIRENTS == 0) {
    parent_node->blocks[parent_node->nlinks / DIRENTS] = alloc_dblock();
    parent_node->size += BLOCK_SIZE;

    if (parent_node->blocks[parent_node->nlinks / DIRENTS] == 0) {
      errno = ENOSPC;
      return -ENOSPC;
    }
  }

  struct wfs_dentry *ents = (struct wfs_dentry *)&data_map[parent_node->blocks[parent_node->nlinks / DIRENTS]];
  struct wfs_dentry *ent = &ents[parent_node->nlinks % DIRENTS];

  strcpy(ent->name, child_str);
  
  off_t inode_number = alloc_inode();
  if (inode_number == 0) {
    errno = ENOSPC;
    return -ENOSPC;
  }
  ent->num = inode_number;

  parent_node->nlinks++;

  struct wfs_inode *new_inode = (struct wfs_inode *) (data_map + sb.i_blocks_ptr + inode_number * BLOCK_SIZE);

  new_inode->uid = getuid();
  new_inode->gid = getgid();
  new_inode->mode = mode;
  new_inode->size = 0;
  new_inode->nlinks = 0;
  new_inode->atim = time(NULL);
  new_inode->mtim = time(NULL);
  new_inode->ctim = time(NULL);

  free(path_cpy);

  return 0;
}

int wfs_mknod(const char *path, mode_t mode, dev_t dev) {
  return add_entry(path, S_IFREG | mode);
}

int wfs_mkdir(const char *path, mode_t mode) {
  return add_entry(path, S_IFDIR | mode);
}

void remove_ent(struct wfs_inode *parent, struct wfs_dentry *ent) {
  inode_bmap[ent->num/8] ^= (1 << (ent->num % 8));

  int repli = parent->nlinks - 1;

  struct wfs_dentry *ents = (struct wfs_dentry *)&data_map[parent->blocks[repli / DIRENTS]];
  struct wfs_dentry *repl = &ents[repli % DIRENTS];

  ent->num = repl->num;
  strcpy(ent->name, repl->name);

  if (repli % DIRENTS == 0 && repli > 0) {
    free_dblock(parent->blocks[repli / DIRENTS]);
  }

  parent->nlinks--;
}

void get_ent(const char *path, struct wfs_inode **parent, struct wfs_dentry **child_ent, struct wfs_inode **child) {
  char *path_cpy = strdup(path);
  
  char *child_str;
  split(path_cpy, &child_str);

  get_inode(path_cpy, parent);

  for (int i = 0; i < (*parent)->nlinks; i++) {
    struct wfs_dentry *ents = (struct wfs_dentry *)&data_map[(*parent)->blocks[i / DIRENTS]];
    struct wfs_dentry *ent = &ents[i % DIRENTS];

    if (!strcmp(ent->name, child_str)) {
      *child_ent = ent;
      *child = (struct wfs_inode *)(data_map + sb.i_blocks_ptr + ent->num * BLOCK_SIZE);
      break;
    }
  }
  
  free(path_cpy);
}

int wfs_unlink(const char *path) {
  struct wfs_inode *parent;
  struct wfs_dentry *ent;
  struct wfs_inode *node;
  get_ent(path, &parent, &ent, &node);

  int has_indirect_block = 0;
  for (int i = 0; i < node->size / BLOCK_SIZE; i++) {
    if (i < IND_BLOCK) {
      free_dblock(node->blocks[i]);
    }
    else {
      has_indirect_block = 1;

      int *ind = (int *)&data_map[node->blocks[IND_BLOCK]];
      free_dblock(ind[i - IND_BLOCK]);
    }
  }

  if (has_indirect_block) {
    free_dblock(node->blocks[IND_BLOCK]);
  }

  remove_ent(parent, ent);

  return 0;
}

int wfs_rmdir(const char *path) {
  struct wfs_inode *parent;
  struct wfs_dentry *ent;
  struct wfs_inode *node;
  get_ent(path, &parent, &ent, &node);

  if (node->blocks[0] > 0)
    free_dblock(node->blocks[0]);

  remove_ent(parent, ent);

  return 0;
}

char *get_block_num(struct wfs_inode *node, int j) {
  char *block;

  int block_num = j / BLOCK_SIZE;

  if (j >= node->size) {
    if (node->size % BLOCK_SIZE == 0) {
      int new_block_num = node->size / BLOCK_SIZE;
      if (new_block_num < IND_BLOCK) {
        node->blocks[new_block_num] = alloc_dblock();
        if (node->blocks[new_block_num] == 0)
          return NULL;
      } else {
        if (new_block_num == IND_BLOCK) {
          node->blocks[IND_BLOCK] = alloc_dblock();
          if (node->blocks[IND_BLOCK] == 0)
            return NULL;
        }
        int *ind = (int *)&data_map[node->blocks[IND_BLOCK]];
        ind[block_num - IND_BLOCK] = alloc_dblock();
        if (ind[block_num - IND_BLOCK] == 0)
          return NULL;
      }
    }
    node->size = j + 1;
  }

  if (block_num < IND_BLOCK) {
    block = &data_map[node->blocks[block_num]];
  } else {
    int *indirect = (int *)&data_map[node->blocks[IND_BLOCK]];
    block = &data_map[indirect[block_num - IND_BLOCK]];
  }

  return block;
}

int wfs_read(const char *path, char *buf, size_t len, off_t off,
             struct fuse_file_info *ffi) {
  struct wfs_inode *node;
  get_inode(path, &node);
  int bytes_read = 0;

  for (int i = 0; i < len; i++) {
    int j = i + off;
    if (j >= node->size)
      break;

    char *cur_block = get_block_num(node, j);

    buf[i] = cur_block[j % BLOCK_SIZE];
    bytes_read++;
  }

  node->atim = time(NULL);

  return bytes_read;
}

int wfs_write(const char *path, const char *buf, size_t len, off_t off,
              struct fuse_file_info *ffi) {
  struct wfs_inode *node;
  get_inode(path, &node);
  int bytes_written = 0;

  for (int i = 0; i < len; i++) {
    int j = i + off;
    
    char *cur_block = get_block_num(node, j);
    if (cur_block == NULL) {
      errno = ENOSPC;
      return -ENOSPC;
    }

    cur_block[j % BLOCK_SIZE] = buf[i];
    bytes_written++;
  }

  node->ctim = time(NULL);
  node->mtim = time(NULL);

  return bytes_written;
}

int wfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                off_t offset, struct fuse_file_info *fi) {
  struct wfs_inode *node;
  get_inode(path, &node);

  if (offset >= node->nlinks) return 0;

  for (int i = offset; i < node->nlinks; i++) {
    struct wfs_dentry *ents = (struct wfs_dentry *)&data_map[node->blocks[i / DIRENTS]];
    struct wfs_dentry *ent = &ents[i % DIRENTS];

    filler(buf, ent->name, NULL, i + 1);
  }

  return 0;
}

static struct fuse_operations ops = {
    .getattr = wfs_getattr,
    .mknod = wfs_mknod,
    .mkdir = wfs_mkdir,
    .unlink = wfs_unlink,
    .rmdir = wfs_rmdir,
    .read = wfs_read,
    .write = wfs_write,
    .readdir = wfs_readdir,
};

int main(int argc, char *argv[]) {
  if (argc < 2)
    return 69;
  char *disk_img = argv[1];

  struct stat st;
  stat(disk_img, &st);

  int fd = open(disk_img, O_RDWR);
  data_map = mmap(NULL, st.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  close(fd);

  sb = *((struct wfs_sb *)data_map);

  inode_bmap = (char *)(data_map + sb.i_bitmap_ptr);
  block_bmap = (char *)(data_map + sb.d_bitmap_ptr);

  argv[1] = argv[0];

  return fuse_main(argc - 1, argv + 1, &ops, NULL);
}
