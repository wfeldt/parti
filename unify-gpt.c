#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <getopt.h>
#include <string.h>
#include <inttypes.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <sys/mount.h>
#include <uuid/uuid.h>

#ifndef VERSION
#define VERSION "0.0"
#endif

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
#define GPT_SIGNATURE   0x5452415020494645ll

#define MIN_BLOCK_SHIFT	9
#define MAX_BLOCK_SHIFT	12
#define BLOCK_SIZES	(MAX_BLOCK_SHIFT - MIN_BLOCK_SHIFT + 1)

#define MIN(a, b)	((a) < (b) ? (a) : (b))
#define MAX(a, b)	((a) > (b) ? (a) : (b))

struct option options[] = {
  { "add",         0, NULL, 'a'  },
  { "normalize",   0, NULL, 'n'  },
  { "list",        0, NULL, 'l'  },
  { "entries",     1, NULL, 'e'  },
  { "block-size",  1, NULL, 'b'  },
  { "version",     0, NULL, 1001 },
  { "help",        0, NULL, 'h'  },
  { }
};

struct {
  unsigned add:1;
  unsigned normalize:1;
  unsigned list:1;
  unsigned verbose;
  unsigned block_shift;
  unsigned entries;
} opt;

typedef struct {
  uint8_t *ptr;
  unsigned len;
} data_t;

typedef struct {
  char *name;
  int fd;
  struct {
    unsigned blockdev:1;
  } is;
  unsigned block_shift;
  uint64_t size;
} disk_t;

typedef struct {
  uint64_t signature;
  uint32_t revision;
  uint32_t header_size;
  uint32_t header_crc;
  uint32_t reserved;
  uint64_t current_lba;
  uint64_t backup_lba;
  uint64_t first_lba;
  uint64_t last_lba;
  uuid_t disk_guid;
  uint64_t partition_lba;
  uint32_t partition_entries;
  uint32_t partition_entry_size;
  uint32_t partition_crc;
} gpt_header_t;

typedef struct {
  uuid_t type_guid;
  uuid_t partition_guid;
  uint64_t first_lba;
  uint64_t last_lba;
  uint64_t attributes;
  uint16_t name[36];
  unsigned zero:1;
  unsigned ok:1;
} gpt_entry_t;

typedef struct {
  data_t header_block;
  data_t entry_blocks;
  gpt_header_t header;
  unsigned used_entries;
  unsigned table_size;			// in bytes
  uint64_t min_used_lba, max_used_lba;
  unsigned block_shift;
  unsigned ok:1;
} gpt_t;

typedef struct {
  data_t pmbr_block;
  gpt_t primary[BLOCK_SIZES];
  gpt_t backup[BLOCK_SIZES];
  uint64_t start_used, end_used;	// in bytes
  unsigned used_entries;
} gpt_list_t;

void help(void);
int get_disk_properties(disk_t *disk);
int get_gpt_list(disk_t *disk, gpt_list_t *gpt_list);
gpt_entry_t get_gpt_entry(gpt_t *gpt, unsigned idx);
gpt_t get_gpt(disk_t *disk, unsigned block_shift, uint64_t start_block);
void free_data(data_t *data);
void free_gpt(gpt_t *gpt);
data_t read_disk(disk_t *disk, uint64_t start, unsigned len);
int write_disk(disk_t *disk, uint64_t start, data_t *data);
data_t clone_data(data_t *data);
void resize_data(data_t *data, unsigned len);
gpt_t clone_gpt(gpt_t *gpt, unsigned block_shift);
int add_gpt(disk_t *disk, gpt_list_t *gpt_list, unsigned block_shift);
int normalize_gpt(disk_t *disk, gpt_list_t *gpt_list, unsigned block_shift);
int calculate_gpt_list(disk_t *disk, gpt_list_t *gpt_list);
int write_pmbr(disk_t *disk, gpt_list_t *gpt_list);
int write_gpt(disk_t *disk, gpt_t *gpt);
int write_gpt_list(disk_t *disk, gpt_list_t *gpt_list);
void update_gpt(gpt_t *gpt);
void update_pmbr(disk_t *disk, gpt_list_t *gpt_list);

uint32_t chksum_crc32(void *buf, unsigned len);

uint16_t get_uint16_le(uint8_t *buf);
uint32_t get_uint32_le(uint8_t *buf);
uint64_t get_uint64_le(uint8_t *buf);

void put_uint16_le(uint8_t *buf, uint16_t val);
void put_uint32_le(uint8_t *buf, uint32_t val);
void put_uint64_le(uint8_t *buf, uint64_t val);

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
int main(int argc, char **argv)
{
  int i;
  extern int optind;
  extern int opterr;

  opterr = 0;

  while((i = getopt_long(argc, argv, "anlhb:e:", options, NULL)) != -1) {
    switch(i) {
      case 'a':
        opt.add = 1;
        break;

      case 'n':
        opt.normalize = 1;
        break;

      case 'l':
        opt.list = 1;
        break;

      case 'e':
        int entries = atoi(optarg);
        if(entries < 4 || entries > 1024) {
          fprintf(stderr, "unsupported number of partition entries: %d\n", entries);
          return 1;
        }
        opt.entries = (unsigned) entries;
        break;

      case 'b':
        int block_size = atoi(optarg);
        for(unsigned u = MIN_BLOCK_SHIFT; u <= MAX_BLOCK_SHIFT; u++) {
          if(block_size == 1 << u) {
            opt.block_shift = u;
            break;
          }
        }
        if(!opt.block_shift) {
          fprintf(stderr, "unsupported block size: %d\n", block_size);
          return 1;
        }
        break;

      case 1001:
        printf(VERSION "\n");
        return 0;
        break;

      default:
        help();
        return i == 'h' ? 0 : 1;
    }
  }

  argc -= optind;
  argv += optind;

  if(argc != 1 || !(opt.list || opt.add || opt.normalize)) {
    help();
    return 1;
  }

  disk_t disk = { .name = argv[0] };

  if(get_disk_properties(&disk)) {
    fprintf(stderr, "%s: failed to get disk properties\n", disk.name);
    return 1;
  }

  gpt_list_t gpt_list = { };  

  if(get_gpt_list(&disk, &gpt_list)) {
    fprintf(stderr, "unsupported partition table setup\n");
    return 1;
  }

  if(opt.list) return 0;

  if(opt.add || opt.normalize) {
    if(opt.add) {
      if(!add_gpt(&disk, &gpt_list, opt.block_shift ?: 12)) return 1;
    }
    else {
      if(!normalize_gpt(&disk, &gpt_list, opt.block_shift)) return 1;
    }
    if(!calculate_gpt_list(&disk, &gpt_list)) {
      fprintf(stderr, "error calculating new gpt layout\n");
      return 1;
    }
    if(!write_gpt_list(&disk, &gpt_list)) {
      fprintf(stderr, "error writing new gpt\n");
      return 1;
    }
    fsync(disk.fd);
    close(disk.fd);
  }

  return 0;
}


void help()
{
  fprintf(stderr,
    "Usage: unify-gpt [OPTIONS] DISK_DEVICE\n"
    "Create a unified GPT for multiple block sizes.\n"
    "\n"
    "Options:\n"
    "  -l, --list            Show current GPT setup.\n"
    "  -a, --add             Add GPT for the specified block size (default: 4096).\n"
    "  -n, --normalize       Normaize GPT. This removes additional GPTs and keeps only\n"
    "                        a single GPT.\n"
    "                        The default block size for block devices is the device block size.\n"
    "                        The default block size for image files is the smallest block size\n"
    "                        for which there is a GPT.\n"
    "  -b, --block-size N    Block size to use. Possible values are 512, 1024, 2048, and 4096.\n"
    "  -e, --entries N       Create GPT with N partition slots (default: 128).\n"
    "  --version             Show version.\n"
    "  --help                Print this help text.\n"
    "\n"
    "This tool takes a disk device or disk image file with a valid GPT and adds a valid GPT\n"
    "for the specified block size. This allows you to have valid GPTs for multiple block sizes.\n"
    "\n"
    "The purpose is to be able to prepare disk images suitable for several block sizes. Once\n"
    "the image is used, remove the extra GPTs using '--normalize'\n"
    "\n"
    "Existing partitions are kept. Partitions must be aligned to the requested block size.\n"
    "\n"
    "You can run the tool several times to support more block sizes.\n"
    "\n"
    "The additional GPTs need extra space and partitioning tools may notify you that not the\n"
    "entire disk space is used.\n"
    "\n"
    "Note: since partitioning tools will update only the GPT for a specific block size, your\n"
    "partition setup will get out of sync. Use the '--normalize' option to remove the extra GPTs\n"
    "and keep only a single GPT for the desired block size before running a partitioning tool.\n"
  );
}



int get_disk_properties(disk_t *disk)
{
  disk->fd = open(disk->name, O_RDWR | O_LARGEFILE);

  if(disk->fd == -1) {
    perror(disk->name);
    return 1;
  }

  struct stat stat = {};

  if(!fstat(disk->fd, &stat)) {
    if((stat.st_mode & S_IFMT) == S_IFBLK) {
      disk->is.blockdev = 1;
    }
    else if((stat.st_mode & S_IFMT) != S_IFREG) {
      return 1;
    }
  }
  else {
    perror(disk->name);
    return 1;
  }

  unsigned block_size = 0;

  if(disk->is.blockdev) {
    if(ioctl(disk->fd, BLKSSZGET, &block_size)) {
      perror(disk->name);
      return 1;
    }

    if(ioctl(disk->fd, BLKGETSIZE64, &disk->size)) {
      perror(disk->name);
      return 1;
    }
  }
  else {
    disk->size = (uint64_t) stat.st_size;
  }

  for(unsigned u = MIN_BLOCK_SHIFT; u <= MAX_BLOCK_SHIFT; u ++) {
    if(block_size == 1u << u) {
      disk->block_shift = u;
    }
  }

  return 0;
}


void free_data(data_t *data)
{
  if(data) {
    if(data->ptr) free(data->ptr);
    data->ptr = 0;
    data->len = 0;
  }
}


void free_gpt(gpt_t *gpt)
{
  if(gpt) {
    free_data(&gpt->header_block);
    free_data(&gpt->entry_blocks);
    *gpt = (gpt_t) { };
  }
}


data_t read_disk(disk_t *disk, uint64_t start, unsigned len)
{
  data_t data = { .len = len };

  // printf("disk read: start = %"PRIu64", len = %u\n", start, len);

  if(lseek(disk->fd, (off_t) start, SEEK_SET) != (off_t) start) {
    perror(disk->name);
    free_data(&data);
    return data;
  }

  data.ptr = malloc(len);

  if(!data.ptr) {
    free_data(&data);
    return data;
  }

  if(read(disk->fd, data.ptr, len) != len) {
    perror(disk->name);
    free_data(&data);
    return data;
  }

  return data;
}


int write_disk(disk_t *disk, uint64_t start, data_t *data)
{
  // printf("disk write: start = %"PRIu64", len = %u\n", start, data->len);

  if(lseek(disk->fd, (off_t) start, SEEK_SET) != (off_t) start) {
    perror(disk->name);
    return 0;
  }

  if(write(disk->fd, data->ptr, data->len) != data->len) {
    perror(disk->name);
    return 0;
  }

  return 1;
}


int get_gpt_list(disk_t *disk, gpt_list_t *gpt_list)
{
  *gpt_list = (gpt_list_t) { };

  unsigned gpts_ok = 0, gpts_bad = 0;

  gpt_list->pmbr_block = read_disk(disk, 0, 1 << MIN_BLOCK_SHIFT);

  if(!gpt_list->pmbr_block.len) return 1;

  gpt_list->start_used--;

  for(unsigned u = 0; u < BLOCK_SIZES; u++) {
    gpt_list->primary[u] = get_gpt(disk, MIN_BLOCK_SHIFT + u, 1);
    if(gpt_list->primary[u].ok) {
      gpt_list->start_used = MIN(gpt_list->start_used, gpt_list->primary[u].min_used_lba << gpt_list->primary[u].block_shift);
      gpt_list->end_used = MAX(gpt_list->end_used, (gpt_list->primary[u].max_used_lba + 1) << gpt_list->primary[u].block_shift);
      gpt_list->used_entries = MAX(gpt_list->used_entries, gpt_list->primary[u].used_entries);

      gpt_list->backup[u] = get_gpt(disk, MIN_BLOCK_SHIFT + u, gpt_list->primary[u].header.backup_lba);
      printf("existing gpt: block size %u, partitions %u", 1 << gpt_list->primary[u].block_shift, gpt_list->primary[u].used_entries);
      if(gpt_list->backup[u].ok) {
        gpts_ok++;
      }
      else {
        gpts_bad++;
        printf(" - but no backup gpt");
      }
      printf("\n");
    }
  }

  // printf("gpt_list: start = %"PRIu64", end = %"PRIu64", entries = %u\n", gpt_list->start_used, gpt_list->end_used, gpt_list->used_entries);

  return gpts_ok && !gpts_bad ? 0 : 1;
}


gpt_entry_t get_gpt_entry(gpt_t *gpt, unsigned idx)
{
  gpt_entry_t entry = { };

  if(
    idx >= gpt->header.partition_entries ||
    gpt->header.partition_entry_size * (idx + 1) > gpt->entry_blocks.len
  ) return entry;

  uint8_t *buf = gpt->entry_blocks.ptr + gpt->header.partition_entry_size * idx;

  memcpy(entry.type_guid, buf, sizeof entry.type_guid);
  memcpy(entry.partition_guid, buf, sizeof entry.partition_guid);
  entry.first_lba = get_uint64_le(buf + 32);
  entry.last_lba = get_uint64_le(buf + 40);
  entry.attributes = get_uint64_le(buf + 48);
  memcpy(entry.name, buf + 56, sizeof entry.name);

  if(entry.first_lba < entry.last_lba) {
    entry.ok = 1;
  }
  else {
    entry.zero = 1;
    for(unsigned u = 0 ; u < gpt->header.partition_entry_size; u++) {
      if(buf[u]) {
        entry.zero = 0;
        break;
      }
    }
  }

  return entry;
}


gpt_t get_gpt(disk_t *disk, unsigned block_shift, uint64_t start_block)
{
  gpt_t gpt = { };

  data_t gpt_header_block = read_disk(disk, start_block << block_shift, 1 << block_shift);

  if(!gpt_header_block.len) return gpt;

  gpt_header_t gpt_h = { };

  gpt_h.signature = get_uint64_le(gpt_header_block.ptr);
  if(gpt_h.signature != GPT_SIGNATURE) {
    free_data(&gpt_header_block);
    return gpt;
  }

  gpt_h.revision = get_uint32_le(gpt_header_block.ptr + 8);
  gpt_h.header_size = get_uint32_le(gpt_header_block.ptr + 12);
  gpt_h.header_crc = get_uint32_le(gpt_header_block.ptr + 16);
  gpt_h.current_lba = get_uint64_le(gpt_header_block.ptr + 24);
  gpt_h.backup_lba = get_uint64_le(gpt_header_block.ptr + 32);
  gpt_h.first_lba = get_uint64_le(gpt_header_block.ptr + 40);
  gpt_h.last_lba = get_uint64_le(gpt_header_block.ptr + 48);
  memcpy(gpt_h.disk_guid, gpt_header_block.ptr + 56, sizeof gpt_h.disk_guid);
  gpt_h.partition_lba = get_uint64_le(gpt_header_block.ptr + 72);
  gpt_h.partition_entries = get_uint32_le(gpt_header_block.ptr + 80);
  gpt_h.partition_entry_size = get_uint32_le(gpt_header_block.ptr + 84);
  gpt_h.partition_crc = get_uint32_le(gpt_header_block.ptr + 88);

  // accept only standard header size and validate header crc32
  unsigned header_ok = 0;

  if(gpt_h.header_size == 92) {
    uint8_t tmp[92];
    memcpy(tmp, gpt_header_block.ptr, sizeof tmp);
    memset(tmp + 16, 0, 4);
    if(chksum_crc32(tmp, sizeof tmp) == gpt_h.header_crc) header_ok = 1;
  }

  if(
    gpt_h.current_lba != start_block ||
    gpt_h.partition_entry_size != 128 ||
    gpt_h.partition_entries < 4 ||
    gpt_h.partition_entries > 1024
  ) {
    header_ok = 0;
  }

  if(!header_ok) {
    free_data(&gpt_header_block);
    return gpt;
  }

  // printf("block_size %u: gpt head ok\n", 1 << block_shift);

  data_t gpt_entry_blocks = read_disk(disk, gpt_h.partition_lba << block_shift, gpt_h.partition_entries * gpt_h.partition_entry_size);

  if(!gpt_entry_blocks.len) {
    free_data(&gpt_header_block);
    return gpt;
  }

  uint32_t partition_crc = chksum_crc32(gpt_entry_blocks.ptr, gpt_entry_blocks.len);

  if(partition_crc != gpt_h.partition_crc) {
    free_data(&gpt_entry_blocks);
    free_data(&gpt_header_block);
    return gpt;
  }

  // printf("gpt entries ok\n");

  gpt.header_block = gpt_header_block;
  gpt.entry_blocks = gpt_entry_blocks;
  gpt.header = gpt_h;
  gpt.block_shift = block_shift;
  gpt.ok = 1;

  gpt.min_used_lba--;
  for(unsigned u = 0; u < gpt_h.partition_entries; u++) {
    gpt_entry_t e = get_gpt_entry(&gpt, u);
    if(e.ok) {
      if(e.first_lba < gpt.min_used_lba) gpt.min_used_lba = e.first_lba;
      if(e.last_lba > gpt.max_used_lba) gpt.max_used_lba = e.last_lba;
    }
    if(!e.zero) gpt.used_entries = u + 1;
  }

  if(!gpt.max_used_lba) gpt.min_used_lba = 0;

  // printf("+++ %u entries, %"PRIu64" - %"PRIu64"\n", gpt.used_entries, gpt.min_used_lba, gpt.max_used_lba);

  return gpt;
}


data_t clone_data(data_t *data)
{
  data_t new_data = { .len = data->len };

  if(!new_data.len) return new_data;

  new_data.ptr = malloc(new_data.len);

  if(!new_data.ptr) {
    new_data.len = 0;
    return new_data;
  }

  memcpy(new_data.ptr, data->ptr, new_data.len);

  return new_data;
}


void resize_data(data_t *data, unsigned len)
{
  if(len <= data->len) {
    data->len = len;
    return;
  }

  data->ptr = realloc(data->ptr, len);

  if(data->ptr) {
    memset(data->ptr + data->len, 0, len - data->len);
    data->len = len;
  }
}


gpt_t clone_gpt(gpt_t *gpt, unsigned block_shift)
{
  gpt_t new_gpt = { };

  new_gpt = *gpt;

  new_gpt.ok = 0;

  new_gpt.header_block = clone_data(&gpt->header_block);
  new_gpt.entry_blocks = clone_data(&gpt->entry_blocks);

  if(!new_gpt.header_block.len || !new_gpt.entry_blocks.len) return new_gpt;

  unsigned block_mask = (1u << block_shift) - 1;

  for(unsigned u = 0; u < new_gpt.used_entries; u++) {
    uint8_t *entry = new_gpt.entry_blocks.ptr + u * new_gpt.header.partition_entry_size;
    // start
    uint64_t lba = get_uint64_le(entry + 32);
    if(lba) {
      lba <<= gpt->block_shift;
      if((lba & block_mask)) {
        fprintf(stderr, "misaligned partition start: %"PRIu64"\n", lba);
        return new_gpt;
      }
      put_uint64_le(entry + 32, lba >> block_shift);
    }

    // end
    lba = get_uint64_le(entry + 40);
    if(lba) {
      lba = (lba + 1) << gpt->block_shift;
      if((lba & block_mask)) {
        fprintf(stderr, "misaligned partition end: %"PRIu64"\n", lba);
        return new_gpt;
      }
      put_uint64_le(entry + 40, (lba - 1) >> block_shift);
    }
  }

  new_gpt.block_shift = block_shift;
  new_gpt.ok = 1;

  return new_gpt;
}


int add_gpt(disk_t *disk, gpt_list_t *gpt_list, unsigned block_shift)
{
  if(gpt_list->primary[block_shift - MIN_BLOCK_SHIFT].ok) {
    fprintf(stderr, "gpt for block size %u already exists\n", 1 << block_shift);
    return 0;
  }

  for(unsigned u = MIN_BLOCK_SHIFT; u <= MAX_BLOCK_SHIFT; u++) {
    gpt_t *gpt = &gpt_list->primary[u - MIN_BLOCK_SHIFT];
    if(!gpt->ok) continue;

    gpt_list->primary[block_shift - MIN_BLOCK_SHIFT] = clone_gpt(gpt, block_shift);
    if(!gpt_list->primary[block_shift - MIN_BLOCK_SHIFT].ok) return 0;
    gpt_list->backup[block_shift - MIN_BLOCK_SHIFT] = clone_gpt(gpt, block_shift);
    if(!gpt_list->backup[block_shift - MIN_BLOCK_SHIFT].ok) return 0;

    break;
  }

  printf("adding gpt: block size %u\n", 1 << block_shift);

  return 1;
}


int normalize_gpt(disk_t *disk, gpt_list_t *gpt_list, unsigned block_shift)
{
  unsigned gpts = 0;

  if(!block_shift) block_shift = disk->block_shift;

  for(unsigned u = MIN_BLOCK_SHIFT; u <= MAX_BLOCK_SHIFT; u++) {
    gpt_t *gpt = &gpt_list->primary[u - MIN_BLOCK_SHIFT];
    if(!gpt->ok) continue;
    gpts++;
    if(!block_shift) block_shift = u;
  }

  if(gpts == 1) {
    fprintf(stderr, "nothing to do: single gpt for block size %u\n", 1 << block_shift);
    return 0;
  }

  if(gpts == 0 || !gpt_list->primary[block_shift - MIN_BLOCK_SHIFT].ok) {
    fprintf(stderr, "gpt for block size %u does not exist\n", 1 << block_shift);
    return 0;
  }

  printf("keeping gpt: block size %u\n", 1 << block_shift);

  for(unsigned u = MIN_BLOCK_SHIFT; u <= MAX_BLOCK_SHIFT; u++) {
    gpt_t *gpt = &gpt_list->primary[u - MIN_BLOCK_SHIFT];
    if(u != block_shift) gpt->ok = 0;
    gpt = &gpt_list->backup[u - MIN_BLOCK_SHIFT];
    if(u != block_shift) gpt->ok = 0;
  }

  return 1;
}


int calculate_gpt_list(disk_t *disk, gpt_list_t *gpt_list)
{
  unsigned entries = opt.entries ?: 128;

  if(entries < gpt_list->used_entries) {
    fprintf(stderr, "new gpt must have at least %u entries\n", gpt_list->used_entries);
    return 0;
  }

  unsigned max_shift = MIN_BLOCK_SHIFT;

  for(unsigned u = MIN_BLOCK_SHIFT; u <= MAX_BLOCK_SHIFT; u++) {
    gpt_t *gpt = &gpt_list->primary[u - MIN_BLOCK_SHIFT];
    if(!gpt->ok) continue;

    max_shift = u;
  }

  unsigned entry_align_mask = (1u << (max_shift - 7)) - 1;
  entries = (entries + entry_align_mask) & ~entry_align_mask;

  uint64_t table_end = disk->size;

  // 1st: backup gpt header location, down from disk end
  for(unsigned u = MIN_BLOCK_SHIFT; u <= MAX_BLOCK_SHIFT; u++) {
    gpt_t *gpt = &gpt_list->primary[u - MIN_BLOCK_SHIFT];
    if(!gpt->ok) continue;

    uint64_t block_mask = (1 << u) - 1u;

    table_end &= ~block_mask;
    table_end -= (1 << u);

    gpt->header.backup_lba = table_end >> u;
  }

  uint64_t table_ofs = 2 << max_shift;

  //2nd: partition_lba up from start for primary gpt, and down from end for backup gpt
  for(unsigned u = MIN_BLOCK_SHIFT; u <= MAX_BLOCK_SHIFT; u++) {
    gpt_t *gpt = &gpt_list->primary[u - MIN_BLOCK_SHIFT];
    if(!gpt->ok) continue;

    // printf("+++ blk %u: disk size = %"PRIu64"\n", 1 << u, disk->size >> u);

    uint64_t block_mask = (1 << u) - 1u;

    table_ofs = (table_ofs + block_mask) & ~block_mask;

    uint64_t table_size = ((entries << 7) + block_mask) & ~block_mask;
    // unsigned real_entries = table_size >> 7;

    // primary

    gpt->header.partition_entries = entries;
    gpt->table_size = table_size;
    gpt->header.current_lba = 1;
    gpt->header.partition_lba = table_ofs >> u;

    resize_data(&gpt->header_block, 1 << u);
    resize_data(&gpt->entry_blocks, table_size);

    if(!gpt->header_block.ptr || !gpt->entry_blocks.ptr) {
      fprintf(stderr, "add gpt: out of memory\n");
      return 0;
    }

    gpt->header.partition_crc = chksum_crc32(gpt->entry_blocks.ptr, gpt->entry_blocks.len);

    // printf("   prim partition_lba = %"PRIu64", table_size = %"PRIu64" (%u)\n", gpt->header.partition_lba, table_size, (unsigned) table_size >> u);

    uint64_t backup_lba = gpt->header.backup_lba;

    // backup

    gpt = &gpt_list->backup[u - MIN_BLOCK_SHIFT];

    gpt->header.partition_entries = entries;
    gpt->table_size = table_size;
    gpt->header.current_lba = backup_lba;
    gpt->header.backup_lba = 1;

    table_end &= ~block_mask;

    gpt->header.partition_lba = (table_end - table_size) >> u;

    resize_data(&gpt->header_block, 1 << u);
    resize_data(&gpt->entry_blocks, table_size);

    if(!gpt->header_block.ptr || !gpt->entry_blocks.ptr) {
      fprintf(stderr, "add gpt: out of memory\n");
      return 0;
    }

    gpt->header.partition_crc = chksum_crc32(gpt->entry_blocks.ptr, gpt->entry_blocks.len);

    // printf("   back partition_lba = %"PRIu64", back lba = %"PRIu64"\n", gpt->header.partition_lba, gpt->header.current_lba);

    table_ofs += table_size;
    table_end -= table_size;
  }

  if(table_ofs > gpt_list->start_used || table_end < gpt_list->end_used) {
    fprintf(stderr, "not enough space for primary gpt\n");
    return 0;
  }

  uint64_t first_free = table_ofs;
  uint64_t end_free = table_end;

  uint64_t align_mask = (1u << max_shift) - 1;

  first_free = (first_free + align_mask) & ~align_mask;
  end_free = end_free & ~align_mask;

  // FIXME: align first_free to 1 MiB?
#if 0
  align_mask = (1 << 20) - 1;
#endif

  // 3rd: set first_lba, last_lba
  for(unsigned u = MIN_BLOCK_SHIFT; u <= MAX_BLOCK_SHIFT; u++) {
    gpt_t *gpt = &gpt_list->primary[u - MIN_BLOCK_SHIFT];
    if(!gpt->ok) continue;

    gpt->header.first_lba = first_free >> u;
    gpt->header.last_lba = (end_free >> u) - 1;

    gpt = &gpt_list->backup[u - MIN_BLOCK_SHIFT];

    gpt->header.first_lba = first_free >> u;
    gpt->header.last_lba = (end_free >> u) - 1;
  }

  // 4th: update fields on disk blocks
  for(unsigned u = MIN_BLOCK_SHIFT; u <= MAX_BLOCK_SHIFT; u++) {
    update_gpt(&gpt_list->primary[u - MIN_BLOCK_SHIFT]);
    update_gpt(&gpt_list->backup[u - MIN_BLOCK_SHIFT]);
  }

  update_pmbr(disk, gpt_list);

  return 1;
}


int write_pmbr(disk_t *disk, gpt_list_t *gpt_list)
{
  data_t pmbr = clone_data(&gpt_list->pmbr_block);

  resize_data(&pmbr, 1 << MAX_BLOCK_SHIFT);

  write_disk(disk, 0, &pmbr);

  free_data(&pmbr);

  return 1;
}


int write_gpt(disk_t *disk, gpt_t *gpt)
{
  int ok = 1;

  if(!gpt->ok) return ok;

  if(!write_disk(disk, gpt->header.current_lba << gpt->block_shift, &gpt->header_block)) {
    ok = 0;
    return ok;
  }

  if(!write_disk(disk, gpt->header.partition_lba << gpt->block_shift, &gpt->entry_blocks)) {
    ok = 0;
    return ok;
  }

  return ok;
}


int write_gpt_list(disk_t *disk, gpt_list_t *gpt_list)
{
  int ok = 1;

  write_pmbr(disk, gpt_list);

  for(unsigned u = MIN_BLOCK_SHIFT; u <= MAX_BLOCK_SHIFT; u++) {
    if(!write_gpt(disk, &gpt_list->primary[u - MIN_BLOCK_SHIFT])) {
      ok = 0;
      break;
    }
    if(!write_gpt(disk, &gpt_list->backup[u - MIN_BLOCK_SHIFT])) {
      ok = 0;
      break;
    }
  }

  return ok;
}


void update_pmbr(disk_t *disk, gpt_list_t *gpt_list)
{
  unsigned min_shift = MAX_BLOCK_SHIFT;
  unsigned gpts = 0;

  for(unsigned u = MAX_BLOCK_SHIFT; u >= MIN_BLOCK_SHIFT; u--) {
    gpt_t *gpt = &gpt_list->primary[u - MIN_BLOCK_SHIFT];
    if(!gpt->ok) continue;

    min_shift = u;
    gpts++;
  }

  uint8_t *buf = gpt_list->pmbr_block.ptr + 446;

  if(buf[4] == 0xee) {
    // CHS + type
    put_uint32_le(buf + 4, 0xffffffee);

    uint64_t size = (disk->size >> min_shift) - 1;

    if(gpts != 1 || size > 0xffffffff) size = 0xffffffff;

    put_uint32_le(buf + 12, size);
  }
}


void update_gpt(gpt_t *gpt)
{
  if(!gpt->ok) return;

  uint8_t *buf = gpt->header_block.ptr;

  put_uint64_le(buf + 24, gpt->header.current_lba);
  put_uint64_le(buf + 32, gpt->header.backup_lba);
  put_uint64_le(buf + 40, gpt->header.first_lba);
  put_uint64_le(buf + 48, gpt->header.last_lba);
  put_uint64_le(buf + 72, gpt->header.partition_lba);
  put_uint32_le(buf + 80, gpt->header.partition_entries);
  put_uint32_le(buf + 88, gpt->header.partition_crc);

  put_uint32_le(buf + 16, 0);
  put_uint32_le(buf + 16, chksum_crc32(buf, 92));
}


uint32_t chksum_crc32(void *buf, unsigned len)
{
  uint8_t *bytes = buf;
  uint32_t crc = -1u;

  while(len--) {
    crc ^= *bytes++;
    for(int i = 0; i < 8; i++) {
      crc = (crc >> 1) ^ 0xedb88320 * (crc & 1);
    }
  }

  return ~crc;
}


uint16_t get_uint16_le(uint8_t *buf)
{
  return (buf[1] << 8) + buf[0];
}


uint32_t get_uint32_le(uint8_t *buf)
{
  return ((uint32_t) buf[3] << 24) + (buf[2] << 16) + (buf[1] << 8) + buf[0];
}


uint64_t get_uint64_le(uint8_t *buf)
{
  return ((uint64_t) get_uint32_le(buf + 4) << 32) + get_uint32_le(buf);
}


void put_uint16_le(uint8_t *buf, uint16_t val)
{
  buf[0] = val;
  buf[1] = val >> 8;
}


void put_uint32_le(uint8_t *buf, uint32_t val)
{
  buf[0] = val;
  buf[1] = val >> 8;
  buf[2] = val >> 16;
  buf[3] = val >> 24;
}


void put_uint64_le(uint8_t *buf, uint64_t val)
{
  put_uint32_le(buf, val);
  put_uint32_le(buf + 4, val >> 32);
}
