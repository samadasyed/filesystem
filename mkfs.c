#include "wfs.h"
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>

int setup_filesystem(char *data, size_t size, struct wfs_sb superBlock, int num_inodes, int num_blocks) {
    size_t required_size = superBlock.d_blocks_ptr + superBlock.num_data_blocks * BLOCK_SIZE;
    if (required_size > size) {
        return 200;
    }

    struct wfs_sb *mapped_superblock = (struct wfs_sb *)data;
    *mapped_superblock = superBlock;

    memset(data + superBlock.i_bitmap_ptr, 0, (num_inodes + 7) / 8);
    memset(data + superBlock.d_bitmap_ptr, 0, (num_blocks + 7) / 8);

    *(data + superBlock.i_bitmap_ptr) |= 0x01;

    struct wfs_inode *root_inode = (struct wfs_inode *)(data + superBlock.i_blocks_ptr);
    root_inode->num = 0;
    root_inode->uid = getuid();
    root_inode->gid = getgid();
    root_inode->mode = S_IFDIR | 0755;
    root_inode->size = 0;
    root_inode->nlinks = 0;
    time_t current_time = time(NULL);
    root_inode->atim = current_time;
    root_inode->mtim = current_time;
    root_inode->ctim = current_time;

    return 0;
}

int main(int argc, char *argv[]) {
  if (argc != 7) {
      return 200;
  }

  char *disk_img = NULL;
  int num_inodes = 0;
  int num_blocks = 0;

  for (int i = 1; i < 7; i += 2) {
      if (i + 1 == argc) {
          return 200;
      }

      char *option = argv[i];
      char *value = argv[i + 1];

      if (strcmp(option, "-d") == 0) {
          if (disk_img != NULL) {
              return 200;
          }
          disk_img = value;
      } else if (strcmp(option, "-i") == 0) {
          if (num_inodes != 0) {
              return 200;
          }
          num_inodes = atoi(value);
      } else if (strcmp(option, "-b") == 0) {
          if (num_blocks != 0) { return 200; }
          num_blocks = atoi(value);
      } else {
          return 200;
      }
  }

  if (disk_img == NULL || num_inodes == 0 || num_blocks == 0) {
      return 200;
  }

  num_blocks = ((num_blocks + 31) / 32) * 32;
  num_inodes = ((num_inodes + 31) / 32) * 32;

  struct wfs_sb superBlock = {
    .num_inodes = num_inodes,
    .num_data_blocks = num_blocks,
    .i_bitmap_ptr = sizeof(struct wfs_sb),
    .d_bitmap_ptr = superBlock.i_bitmap_ptr + num_inodes / 8,
    .i_blocks_ptr = superBlock.d_bitmap_ptr + num_blocks / 8,
    .d_blocks_ptr = superBlock.i_blocks_ptr + num_inodes * BLOCK_SIZE,
  };

  struct stat s;
  stat(disk_img, &s);
  size_t size = s.st_size;

  int fd = open(disk_img, O_RDWR);
  char *data = mmap(NULL, s.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

  setup_filesystem(data, size, superBlock, num_inodes, num_blocks);
   
  return 0;
}
