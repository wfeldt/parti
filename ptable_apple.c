#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <iconv.h>
#include <getopt.h>
#include <inttypes.h>
#include <uuid/uuid.h>
#include <blkid/blkid.h>
#include <json-c/json.h>

#include "disk.h"
#include "filesystem.h"
#include "util.h"

#include "ptable_apple.h"

void dump_apple_ptables(disk_t *disk)
{
  for(disk->block_size = 0x200; disk->block_size <= 0x1000; disk->block_size <<= 1) {
    if(dump_apple_ptable(disk)) return;
  }

  // printf("no apple partition table\n");
}


int dump_apple_ptable(disk_t *disk)
{
  int i, parts;
  unsigned u1, u2;
  unsigned char buf[disk->block_size];
  apple_entry_t *apple;
  char *s;

  i = disk_read(disk, buf, 1, 1);

  apple = (apple_entry_t *) buf;

  if(i || be16toh(apple->signature) != APPLE_MAGIC) return 0;

  parts = be32toh(apple->partitions);

  printf(SEP "\napple partition table: %d entries\n", parts);
  printf("  sector size: %d\n", disk->block_size);

  for(i = 1; i <= parts; i++) {
    if(disk_read(disk, buf, i, 1)) break;
    apple = (apple_entry_t *) buf;
    u1 = be32toh(apple->start);
    u2 = be32toh(apple->size);
    printf("%3d  %u - %llu (size %u)", i, u1, (unsigned long long) u1 + u2, u2);
    u1 = be32toh(apple->data_start);
    u2 = be32toh(apple->data_size);
    printf(", rel. %u - %llu (size %u)\n", u1, (unsigned long long) u1 + u2, u2);

    s = cname(apple->type, sizeof apple->type);
    printf("     type[%d] \"%s\"", (int) strlen(s), s);

    printf(", status 0x%x\n", be32toh(apple->status));

    s = cname(apple->name, sizeof apple->name);
    printf("     name[%d] \"%s\"\n", (int) strlen(s), s);

    dump_fs(disk, 5, be32toh(apple->start));
  }

  return 1;
}


