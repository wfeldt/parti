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

#include "eltorito.h"

#define ISO_MAGIC       "\001CD001\001"
#define ELTORITO_MAGIC  "\000CD001\001EL TORITO SPECIFICATION"

// generate output as if we have 512 byte block size
// set to 0 or 2
#define BLK_FIX		2

void dump_eltorito(disk_t *disk)
{
  int i, j, sum;
  unsigned char buf[disk->block_size = 0x800];
  unsigned char zero[32];
  unsigned catalog;
  eltorito_t *el;
  char *s;
  static char *bt[] = {
    "no emulation", "1.2MB floppy", "1.44MB floppy", "2.88MB floppy", "hard disk", ""
  };

  memset(zero, 0, sizeof zero);

  i = disk_read(disk, buf, 0x10, 1);

  if(i || memcmp(buf, ISO_MAGIC, sizeof ISO_MAGIC - 1)) return;

  i = disk_read(disk, buf, 0x11, 1);

  if(i || memcmp(buf, ELTORITO_MAGIC, sizeof ELTORITO_MAGIC - 1)) {
    // printf("no el torito data\n");
    return;
  }

  catalog = le32toh(* (uint32_t *) (buf + 0x47));

  printf(SEP "\nel torito:\n");

  printf("  sector size: %d\n", disk->block_size >> BLK_FIX);

  printf("  boot catalog: %u\n", catalog << BLK_FIX);

  i = disk_read(disk, buf, catalog, 1);

  if(i) return;

  for(i = 0; i < disk->block_size/32; i++) {
    el = (eltorito_t *) (buf + 32 * i);
    if(!memcmp(buf + 32 * i, zero, 32)) continue;
    switch(el->any.header_id) {
      case 0x01:
        printf("  %-3d  type 0x%02x (validation entry)\n", i, el->validation.header_id);

        for(sum = j = 0; j < 16; j++) {
          sum += buf[32 * i + 2 * j] + (buf[32 * i + 2 * j + 1] << 8);
        }
        sum &= 0xffff;

        j = le16toh(el->validation.magic);

        printf("       platform id 0x%02x", el->validation.platform_id);
        printf(", crc 0x%04x (%s)", sum, sum ? "wrong" : "ok");
        printf(", magic %s\n", j == 0xaa55 ? "ok" : "wrong");

        s = cname(el->validation.name, sizeof el->validation.name);
        printf("       manufacturer[%d] \"%s\"\n", (int) strlen(s), s);

        break;

      case 0x44:
        printf("  %-3d  type 0x%02x (section entry extension)\n", i, el->any.header_id);
        printf("       info 0x%02x\n", el->extension.info);
        break;

      case 0x00:
      case 0x88:
        printf("  %-3d%c type 0x%02x (initial/default entry)\n", i, el->any.header_id ? '*' : ' ', el->any.header_id);
        s = bt[el->entry.media < 5 ? el->entry.media : 5];
        printf("       boot type %d (%s)\n", el->entry.media, s);
        printf("       load address 0x%05x", le16toh(el->entry.load_segment) << 4);
        printf(", system type 0x%02x\n", el->entry.system);
        printf("       start %d, size %d%s",
          le32toh(el->entry.start) << BLK_FIX,
          le16toh(el->entry.size),
          BLK_FIX ? "" : "/4"
        );
        if((s = iso_block_to_name(disk, le32toh(el->entry.start) << 2))) {
          printf(", \"%s\"", s);
        }
        s = cname(el->entry.name, sizeof el->entry.name);
        printf("\n       selection criteria 0x%02x \"%s\"\n", el->entry.criteria, s);
        // FIXME!!!
        // dump_fs(disk, 7, le32toh(el->entry.start));
        break;

      case 0x90:
      case 0x91:
        printf(
          "  %-3d  type 0x%02x (%ssection header)\n",
          i,
          el->any.header_id,
          el->any.header_id == 0x91 ? "last " : ""
        );
        printf("       platform id 0x%02x", el->section.platform_id);
        s = cname(el->section.name, sizeof el->section.name);
        printf(", name[%d] \"%s\"\n", (int) strlen(s), s);
        printf("       entries %d\n", le16toh(el->section.entries));
        break;

      default:
        printf("  %-3d  type 0x%02x\n", i, el->any.header_id);
        break;
    }
  }
}
#undef BLK_FIX


