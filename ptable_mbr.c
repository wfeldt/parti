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

#include "ptable_mbr.h"

// FIXME
struct { unsigned verbose:1; struct { unsigned raw:1; } show; } opt;


unsigned cs2s(unsigned cs)
{
  return cs & 0x3f;
}


unsigned cs2c(unsigned cs)
{
  return ((cs >> 8) & 0xff) + ((cs & 0xc0) << 2);
}


void parse_ptable(void *buf, unsigned addr, ptable_t *ptable, unsigned base, unsigned ext_base, int entries)
{
  unsigned u;

  memset(ptable, 0, entries * sizeof *ptable);

  for(; entries; entries--, addr += 0x10, ptable++) {
    ptable->idx = 4 - entries + 1;
    if(read_qword_le(buf + addr) == 0 && read_qword_le(buf + addr + 0x8) == 0) {
      ptable->empty = 1;
    }
    u = read_byte(buf + addr);
    if(u & 0x7f) continue;
    ptable->boot = u >> 7;
    ptable->type = read_byte(buf + addr + 4);
    u = read_word_le(buf + addr + 2);
    ptable->start.c = cs2c(u);
    ptable->start.s = cs2s(u);
    ptable->start.h = read_byte(buf + addr + 1);
    ptable->start.lin = read_dword_le(buf + addr + 8);
    u = read_word_le(buf + addr + 6);
    ptable->end.c = cs2c(u);
    ptable->end.s = cs2s(u);
    ptable->end.h = read_byte(buf + addr + 5);
    ptable->end.lin = ptable->start.lin + read_dword_le(buf + addr + 0xc);

    ptable->real_base = base;
    ptable->base = is_ext_ptable(ptable) ? ext_base : base;

    if(ptable->end.lin != ptable->start.lin && ptable->start.s && ptable->end.s) {
      ptable->valid = 1;
      ptable->end.lin--;
    }
  }
}


int guess_geo(ptable_t *ptable, int entries, unsigned *s, unsigned *h)
{
  unsigned sectors, heads, u, c;
  int i, ok, cnt;

  for(sectors = 63; sectors; sectors--) {
    for(heads = 255; heads; heads--) {
      ok = 1;
      for(cnt = i = 0; i < entries; i++) {
        if(!ptable[i].valid) continue;

        if(ptable[i].start.h >= heads) { ok = 0; break; }
        if(ptable[i].start.s > sectors) { ok = 0; break; }
        if(ptable[i].start.c >= 1023) {
          c = ((ptable[i].start.lin + 1 - ptable[i].start.s)/sectors - ptable[i].start.h)/heads;
          if(c < 1023) { ok = 0; break; }
        }
        else {
          c = ptable[i].start.c;
        }
        u = (c * heads + ptable[i].start.h) * sectors + ptable[i].start.s - 1;
        if(u != ptable[i].start.lin) {
          ok = 0;
          break;
        }
        cnt++;
        if(ptable[i].end.h >= heads) { ok = 0; break; }
        if(ptable[i].end.s > sectors) { ok = 0; break; }
        if(ptable[i].end.c >= 1023) {
          c = ((ptable[i].end.lin + 1 - ptable[i].end.s)/sectors - ptable[i].end.h)/heads;
          if(c < 1023) { ok = 0; break; }
        }
        else {
          c = ptable[i].end.c;
        }
        u = (c * heads + ptable[i].end.h) * sectors + ptable[i].end.s - 1;
        if(u != ptable[i].end.lin) {
          ok = 0;
          break;
        }
        cnt++;
      }
      if(!cnt) ok = 0;
      if(ok) break;
    }
    if(ok) break;
  }

  if(ok) {
    *h = heads;
    *s = sectors;
  }

  return ok;
}


void print_ptable_entry(disk_t *disk, int nr, ptable_t *ptable)
{
  unsigned u;

  if(ptable->valid) {
    if(nr > 4 && is_ext_ptable(ptable)) {
      if(!opt.verbose) return;
      printf("    >");
    }
    else {
      printf("  %-3d", nr);
    }

    u = opt.show.raw ? 0 : ptable->base;

    printf("%c %u - %u (size %u), chs %u/%u/%u - %u/%u/%u",
      ptable->boot ? '*' : ' ',
      ptable->start.lin + u,
      ptable->end.lin + u,
      ptable->end.lin - ptable->start.lin + 1,
      ptable->start.c, ptable->start.h, ptable->start.s,
      ptable->end.c, ptable->end.h, ptable->end.s
    );

    if(opt.verbose) {
      printf(", [ref %d.%d]", ptable->real_base, ptable->idx);
    }

    if(opt.show.raw && ptable->base) printf(", ext base %+d", ptable->base);
    printf("\n");

    printf("       type 0x%02x", ptable->type);
    char *s = mbr_partition_type(ptable->type);
    if(s) printf(" (%s)", s);
    printf("\n");
    fs_detail(disk, 7, (unsigned long long) ptable->start.lin + ptable->base);
  }
  else if(!ptable->empty) {
    printf("  %-3d  invalid data\n", nr);
  }
}


int is_ext_ptable(ptable_t *ptable)
{
  return ptable->type == 5 || ptable->type == 0xf;
}


ptable_t *find_ext_ptable(ptable_t *ptable, int entries)
{
  for(; entries; entries--, ptable++) {
    if(ptable->valid && is_ext_ptable(ptable)) return ptable;
  }
  return NULL;
}


void dump_mbr_ptable(disk_t *disk)
{
  int i, j, pcnt, link_count;
  ptable_t ptable[4], *ptable_ext;
  unsigned s, h, ext_base, id;
  unsigned char buf[disk->block_size];

  i = disk_read(disk, buf, 0, 1);

  if(i || read_word_le(buf + 0x1fe) != 0xaa55) {
    // printf("no mbr partition table\n");
    return;
  }

  for(i = j = 0; i < 4 * 16; i++) {
    j |= read_byte(buf + 0x1be + i);
  }

  // empty partition table
  if(!j) return;

  parse_ptable(buf, 0x1be, ptable, 0, 0, 4);
  i = guess_geo(ptable, 4, &s, &h);
  if(!i) {
    s = 63;
    h = 255;
  }
  disk->sectors = s;
  disk->heads = h;
  disk->cylinders = disk->size / (disk->sectors * disk->heads);

  id = read_dword_le(buf + 0x1b8);
  printf(SEP "\nmbr id: 0x%08x\n", id);

  printf("  sector size: %u\n", disk->block_size);

  if(memmem(buf, disk->block_size, "isolinux.bin", sizeof "isolinux.bin" - 1)) {
    char *s;
    unsigned start = le32toh(*(uint32_t *) (buf + 0x1b0));
    printf("  isolinux.bin: %u", start);
    if((s = iso_block_to_name(disk, start))) {
      printf(", \"%s\"", s);
    }
    printf("\n");
  }

  printf(
    "  mbr partition table (chs %u/%u/%u%s):\n",
    disk->cylinders,
    disk->heads,
    disk->sectors,
    i ? "" : ", inconsistent geo"
  );

  for(i = 0; i < 4; i++) {
    print_ptable_entry(disk, i + 1, ptable + i);
  }

  pcnt = 5;

  link_count = 0;
  ext_base = 0;

  while((ptable_ext = find_ext_ptable(ptable, 4))) {
    if(!link_count++) {
      ext_base = ptable_ext->start.lin;
    }
    // arbitrary, but we don't want to loop forever
    if(link_count > 10000) {
      printf("too many partitions\n");
      break;
    }
    j = disk_read(disk, buf, ptable_ext->start.lin + ptable_ext->base, 1);
    if(j || read_word_le(buf + 0x1fe) != 0xaa55) {
      if(j) printf("disk read error - ");
      printf("not a valid extended partition\n");
      break;
    }
    parse_ptable(buf, 0x1be, ptable, ptable_ext->start.lin + ptable_ext->base, ext_base, 4);
    for(i = 0; i < 4; i++) {
      print_ptable_entry(disk, pcnt, ptable + i);
      if(ptable[i].valid && !is_ext_ptable(ptable + i)) pcnt++;
    }
  }
}


char *mbr_partition_type(unsigned id)
{
  static struct {
    unsigned id;
    char *name;
  } types[] = {
    { 0x00, "empty" },
    { 0x01, "fat12" },
    { 0x04, "fat16 <32mb" },
    { 0x05, "extended" },
    { 0x06, "fat16" },
    { 0x07, "ntfs" },
    { 0x0b, "fat32" },
    { 0x0c, "fat32 lba" },
    { 0x0e, "fat16 lba" },
    { 0x0f, "extended lba" },
    { 0x11, "fat12 hidden" },
    { 0x14, "fat16 <32mb hidden" },
    { 0x16, "fat16 hidden" },
    { 0x17, "ntfs hidden" },
    { 0x1b, "fat32 hidden" },
    { 0x1c, "fat32 lba hidden" },
    { 0x1e, "fat16 lba hidden" },
    { 0x41, "prep" },
    { 0x82, "swap" },
    { 0x83, "linux" },
    { 0x8e, "lvm" },
    { 0x96, "chrp iso9660" },
    { 0xde, "dell utility" },
    { 0xee, "gpt" },
    { 0xef, "efi" },
    { 0xfd, "linux raid" },
  };

  int i;

  for(i = 0; i < sizeof types / sizeof *types; i++) {
    if(types[i].id == id) return types[i].name;

  }

  return NULL;
}
