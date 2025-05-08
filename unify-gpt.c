#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <getopt.h>
#include <string.h>
#include <inttypes.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <uuid/uuid.h>

#ifndef VERSION
#define VERSION "0.0"
#endif

#define GPT_SIGNATURE   0x5452415020494645ll

#define MIN_BLOCK_SHIFT	9
#define MAX_BLOCK_SHIFT	12
#define BLOCK_SIZES	(MAX_BLOCK_SHIFT - MIN_BLOCK_SHIFT + 1)

#define MIN(a, b)	((a) < (b) ? (a) : (b))
#define MAX(a, b)	((a) > (b) ? (a) : (b))


struct option options[] = {
  { "add",         0, NULL, 'a'  },
  { "block-size",  1, NULL, 'b'  },
  { "entries",     1, NULL, 'e'  },
  { "help",        0, NULL, 'h'  },
  { "list",        0, NULL, 'l'  },
  { "normalize",   0, NULL, 'n'  },
  { "verbose",     0, NULL, 'v'  },
  { "version",     0, NULL, 1001 },
  { "overlap",     0, NULL, 1002 },
  { "no-overlap",  0, NULL, 1003 },
  { "align-1m",    0, NULL, 1004 },
  { "no-align-1m", 0, NULL, 1005 },
  { "force",       0, NULL, 1006 },
  { "try",         0, NULL, 1007 },
  { }
};

struct {
  unsigned add:1;
  unsigned normalize:1;
  unsigned list:1;
  unsigned overlap:1;
  unsigned align_1m:1;
  unsigned force:1;
  unsigned _try:1;
  unsigned help:1;
  unsigned verbose;
  unsigned block_shift;
  unsigned entries;
} opt = { .overlap = 1 };


typedef struct {
  uint8_t *ptr;
  unsigned len;
} data_t;

typedef struct {
  uint64_t start;			// bytes
  data_t data;
} cache_t;

typedef struct {
  char *name;
  int fd;
  struct {
    unsigned blockdev:1;
  } is;
  unsigned block_shift;
  uint64_t size;			// bytes
  cache_t cache[2];			// for primary & backup gpt
} disk_t;

typedef struct {
  uint64_t signature;
  uint32_t revision;
  uint32_t header_size;			// bytes
  uint32_t header_crc;
  uint32_t reserved;
  uint64_t current_lba;
  uint64_t backup_lba;
  uint64_t first_lba;
  uint64_t last_lba;
  uuid_t disk_guid;
  uint64_t partition_lba;
  uint32_t partition_entries;
  uint32_t partition_entry_size;	// bytes
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
  unsigned table_size;			// bytes
  uint64_t min_used_lba, max_used_lba;
  unsigned block_shift;
  unsigned next_block_shift;
  unsigned ok:1;
} gpt_t;

typedef struct {
  data_t pmbr_block;
  gpt_t primary[BLOCK_SIZES];
  gpt_t backup[BLOCK_SIZES];
  uint64_t start_used, end_used;	// bytes
  uint64_t primary_end;			// bytes
  uint64_t backup_start;		// bytes
  unsigned used_entries;
} gpt_list_t;


void help(void);
int get_disk_properties(disk_t *disk);
void free_data(data_t *data);
void free_gpt(gpt_t *gpt);
data_t read_disk(disk_t *disk, uint64_t start, unsigned len);
int write_disk(disk_t *disk, uint64_t start, data_t *data);
int flush_cache(disk_t *disk);
int write_cache(disk_t *disk, uint64_t start, data_t *data);
data_t clone_data(data_t *data);
void resize_data(data_t *data, unsigned len);
gpt_t clone_gpt(gpt_t *gpt, unsigned block_shift);
int get_gpt_list(disk_t *disk, gpt_list_t *gpt_list);
gpt_t get_gpt(disk_t *disk, unsigned block_shift, uint64_t start_block);
gpt_entry_t get_gpt_entry(gpt_t *gpt, unsigned idx);
int add_gpt(disk_t *disk, gpt_list_t *gpt_list, unsigned block_shift);
int normalize_gpt(disk_t *disk, gpt_list_t *gpt_list, unsigned block_shift);
int calculate_gpt_list(disk_t *disk, gpt_list_t *gpt_list);
int write_gpt(disk_t *disk, gpt_t *gpt);
int write_gpt_list(disk_t *disk, gpt_list_t *gpt_list);
void update_gpt(gpt_t *gpt);
void update_pmbr(disk_t *disk, gpt_list_t *gpt_list);
uint32_t chksum_crc32(void *buf, unsigned len);
uint32_t get_uint32_le(uint8_t *buf);
uint64_t get_uint64_le(uint8_t *buf);
void put_uint32_le(uint8_t *buf, uint32_t val);
void put_uint64_le(uint8_t *buf, uint64_t val);
uint64_t align_down(uint64_t val, unsigned bits);
uint64_t align_up(uint64_t val, unsigned bits);


int main(int argc, char **argv)
{
  for(int i = opterr = 0; (i = getopt_long(argc, argv, "ab:e:hlnv", options, NULL)) != -1; ) {
    switch(i) {
      case 'a':
        opt.add = 1;
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

      case 'e':
        int entries = atoi(optarg);
        if(entries < 4 || entries > 1024) {
          fprintf(stderr, "unsupported number of partition entries: %d\n", entries);
          return 1;
        }
        opt.entries = (unsigned) entries;
        break;

      case 'f':
        opt.force = 1;
        break;

      case 'h':
        opt.help = 1;
        break;

      case 'l':
        opt.list = 1;
        break;

      case 'n':
        opt.normalize = 1;
        break;

      case 'v':
        opt.verbose++;
        break;

      case 1001:
        printf(VERSION "\n");
        return 0;
        break;

      case 1002:
        opt.overlap = 1;
        break;

      case 1003:
        opt.overlap = 0;
        break;

      case 1004:
        opt.align_1m = 1;
        break;

      case 1005:
        opt.align_1m = 0;
        break;

      case 1006:
        opt.force = 1;
        break;

      case 1007:
        opt._try = 1;
        break;

      default:
        help();
        return 1;
    }
  }

  argc -= optind;
  argv += optind;

  if(opt.help) {
    help();
    return 0;
  }

  if(argc != 1 || !(opt.list || opt.add || opt.normalize)) {
    help();
    return 1;
  }

  disk_t disk = { .name = argv[0] };

  if(!get_disk_properties(&disk)) {
    fprintf(stderr, "%s: failed to get disk properties\n", disk.name);
    return 1;
  }

  gpt_list_t gpt_list = { };  

  if(!get_gpt_list(&disk, &gpt_list)) {
    fprintf(stderr, "unsupported partition table setup\n");
    return 1;
  }

  if(opt.list) return 0;

  if(opt.add || opt.normalize) {
    if(opt.add) {
      if(!add_gpt(&disk, &gpt_list, opt.block_shift ?: 12)) return 1;
    }
    if(opt.normalize) {
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
    "                        Decrease the value if there is not enough free space on disk.\n"
    "  -v, --verbose         Increase log level.\n"
    "  --version             Show version.\n"
    "  --help                Print this help text.\n"
    "\n"
    "%s"
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
    "Note 1: since partitioning tools will update only the GPT for a specific block size, your\n"
    "partition setup will get out of sync. Use the '--normalize' option to remove the extra GPTs\n"
    "and keep only a single GPT for the desired block size before running a partitioning tool.\n"
    , opt.verbose >= 1 ?
    "Extended options:\n"
    "  --force               If partition ends are not aligned for a new block size, round up.\n"
    "                        Note that the partition size is only adjusted in the GPT for the\n"
    "                        requested new block size.\n"
    "  --overlap             Layout backup GPT so that header blocks overlap. This ensures that\n"
    "                        the backup GPT header is in the last disk block (default).\n"
    "  --no-overlap          Layout backup GPT so that there is a separate header block for each\n"
    "                        block size.\n"
    "  --align-1m            Align start of usable space to 1 MiB boundary.\n"
    "  --no-align-1m         Maximize usable space (default).\n"
    "\n"
    : ""
  );
}


int get_disk_properties(disk_t *disk)
{
  disk->fd = open(disk->name, O_RDWR | O_LARGEFILE);

  if(disk->fd == -1) {
    perror(disk->name);
    return 0;
  }

  struct stat stat = {};

  if(!fstat(disk->fd, &stat)) {
    if((stat.st_mode & S_IFMT) == S_IFBLK) {
      disk->is.blockdev = 1;
    }
    else if((stat.st_mode & S_IFMT) != S_IFREG) {
      return 0;
    }
  }
  else {
    perror(disk->name);
    return 0;
  }

  unsigned block_size = 0;

  if(disk->is.blockdev) {
    if(ioctl(disk->fd, BLKSSZGET, &block_size)) {
      perror(disk->name);
      return 0;
    }

    if(ioctl(disk->fd, BLKGETSIZE64, &disk->size)) {
      perror(disk->name);
      return 0;
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

  return 1;
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

  if(opt.verbose >= 3) printf("reading from disk: %u bytes at %"PRIu64"\n", len, start);

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
  if(opt.verbose >= 2) printf("writing to disk: %u bytes at %"PRIu64"\n", data->len, start);

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


int flush_cache(disk_t *disk)
{
  for(unsigned u = 0; u < sizeof disk->cache / sizeof *disk->cache; u++) {
    cache_t *cache = &disk->cache[u];
    if(!write_disk(disk, cache->start, &cache->data)) return 0;
  }

  return 1;
}


int write_cache(disk_t *disk, uint64_t start, data_t *data)
{
  if(opt.verbose >= 3) printf("writing to cache: %u bytes at %"PRIu64"\n", data->len, start);

  for(unsigned u = 0; u < sizeof disk->cache / sizeof *disk->cache; u++) {
    cache_t *cache = &disk->cache[u];
    if(start >= cache->start) {
      start -= cache->start;
      if(start + data->len <= cache->data.len) {
        memcpy(cache->data.ptr + start, data->ptr, data->len);
        return 1;
      }
    }
  }

  fprintf(stderr, "oops: invalid write attempt prevented\n");

  return 0;
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
        fprintf(stderr, "gpt_%u: partition %u has misaligned start\n", 1 << block_shift, u + 1);
        return new_gpt;
      }
      put_uint64_le(entry + 32, lba >> block_shift);
    }

    // end
    lba = get_uint64_le(entry + 40);
    if(lba) {
      lba = (lba + 1) << gpt->block_shift;
      if((lba & block_mask)) {
        fprintf(stderr, "gpt_%u: partition %u has misaligned end%s\n",
          1 << block_shift, u + 1, opt.force ? " - rounding up" : " - use option '--force' to fix"
        );
        if(opt.force) {
          lba = align_up(lba, block_shift);
        }
        else {
          return new_gpt;
        }
      }
      put_uint64_le(entry + 40, (lba - 1) >> block_shift);
    }
  }

  new_gpt.block_shift = block_shift;
  new_gpt.ok = 1;

  return new_gpt;
}


int get_gpt_list(disk_t *disk, gpt_list_t *gpt_list)
{
  *gpt_list = (gpt_list_t) { };

  unsigned gpts_ok = 0, gpts_bad = 0;

  gpt_list->pmbr_block = read_disk(disk, 0, 1 << MIN_BLOCK_SHIFT);

  if(!gpt_list->pmbr_block.len) return 0;

  gpt_list->start_used--;

  for(unsigned u = 0; u < BLOCK_SIZES; u++) {
    gpt_list->primary[u] = get_gpt(disk, MIN_BLOCK_SHIFT + u, 1);
    if(gpt_list->primary[u].ok) {
      gpt_list->start_used = MIN(gpt_list->start_used, gpt_list->primary[u].min_used_lba << gpt_list->primary[u].block_shift);
      gpt_list->end_used = MAX(gpt_list->end_used, (gpt_list->primary[u].max_used_lba + 1) << gpt_list->primary[u].block_shift);
      gpt_list->used_entries = MAX(gpt_list->used_entries, gpt_list->primary[u].used_entries);

      gpt_list->backup[u] = get_gpt(disk, MIN_BLOCK_SHIFT + u, gpt_list->primary[u].header.backup_lba);
      printf("found gpt_%u: %u partitions", 1 << gpt_list->primary[u].block_shift, gpt_list->primary[u].used_entries);
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

  return gpts_ok && !gpts_bad ? 1 : 0;
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
    put_uint32_le(tmp + 16, 0);
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

  // printf("+++ %u entries, %"PRIu64" - %"PRIu64"\n", gpt.used_entries, gpt.min_used_lba, gpt.max_used_lba);

  return gpt;
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

  printf("adding gpt_%u\n", 1 << block_shift);

  return 1;
}


int normalize_gpt(disk_t *disk, gpt_list_t *gpt_list, unsigned block_shift)
{
  unsigned gpts = 0;
  unsigned gpt_block_shift = 0;

  if(!block_shift) block_shift = disk->block_shift;

  for(unsigned u = MIN_BLOCK_SHIFT; u <= MAX_BLOCK_SHIFT; u++) {
    gpt_t *gpt = &gpt_list->primary[u - MIN_BLOCK_SHIFT];
    if(!gpt->ok) continue;
    gpts++;
    if(!gpt_block_shift) gpt_block_shift = u;
    if(!block_shift) block_shift = u;
  }

  if(gpts == 1 && !opt.force && !opt.entries) {
    fprintf(stderr, "nothing to do: single gpt for block size %u\n", 1 << gpt_block_shift);
    return 0;
  }

  if(gpts == 0 || !gpt_list->primary[block_shift - MIN_BLOCK_SHIFT].ok) {
    fprintf(stderr, "gpt for block size %u does not exist\n", 1 << block_shift);
    return 0;
  }

  for(unsigned u = MIN_BLOCK_SHIFT; u <= MAX_BLOCK_SHIFT; u++) {
    gpt_t *gpt = &gpt_list->primary[u - MIN_BLOCK_SHIFT];
    if(gpt->ok && u != block_shift) {
      printf("deleting gpt_%u\n", 1 << u);
      gpt->ok = 0;
    }
    gpt = &gpt_list->backup[u - MIN_BLOCK_SHIFT];
    if(gpt->ok && u != block_shift) gpt->ok = 0;
  }

  printf("keeping gpt_%u\n", 1 << block_shift);

  return 1;
}


int calculate_gpt_list(disk_t *disk, gpt_list_t *gpt_list)
{
  unsigned entries = opt.entries ?: 128;

  entries = MAX(entries, gpt_list->used_entries);

  unsigned max_shift = MIN_BLOCK_SHIFT;

  unsigned last_idx = 0;
  for(unsigned u = MIN_BLOCK_SHIFT; u <= MAX_BLOCK_SHIFT; u++) {
    gpt_t *gpt = &gpt_list->primary[u - MIN_BLOCK_SHIFT];
    if(!gpt->ok) continue;

    gpt->next_block_shift = gpt->block_shift;
    if(last_idx) gpt_list->primary[last_idx - MIN_BLOCK_SHIFT].next_block_shift = gpt->block_shift;
    last_idx = u;

    max_shift = u;
  }

  uint64_t table_end = disk->size;

  // 1st: backup gpt header location, down from disk end
  for(unsigned u = MIN_BLOCK_SHIFT; u <= MAX_BLOCK_SHIFT; u++) {
    gpt_t *gpt = &gpt_list->primary[u - MIN_BLOCK_SHIFT];
    if(!gpt->ok) continue;

    table_end = align_down(opt.overlap ? disk->size : table_end, u);
    table_end -= 1 << u;

    gpt->header.backup_lba = table_end >> u;
  }

  uint64_t table_ofs = 2 << max_shift;

  //2nd: partition_lba up from start for primary gpt, and down from end for backup gpt
  for(unsigned u = MIN_BLOCK_SHIFT; u <= MAX_BLOCK_SHIFT; u++) {
    gpt_t *gpt = &gpt_list->primary[u - MIN_BLOCK_SHIFT];
    if(!gpt->ok) continue;

    // printf("+++ blk %u: disk size = %"PRIu64"\n", 1 << u, disk->size >> u);

    table_ofs = align_up(table_ofs, u);

    uint64_t table_size = align_up(entries << 7, gpt->next_block_shift);
    unsigned real_entries = table_size >> 7;

    // primary

    gpt->header.partition_entries = real_entries;
    gpt->table_size = table_size;
    gpt->header.current_lba = 1;
    gpt->header.partition_lba = table_ofs >> u;

    resize_data(&gpt->header_block, 1 << u);
    resize_data(&gpt->entry_blocks, table_size);

    if(!gpt->header_block.ptr || !gpt->entry_blocks.ptr) {
      fprintf(stderr, "malloc: out of memory\n");
      return 0;
    }

    gpt_list->primary_end = table_ofs;

    gpt->header.partition_crc = chksum_crc32(gpt->entry_blocks.ptr, gpt->entry_blocks.len);

    // printf("   prim partition_lba = %"PRIu64", table_size = %"PRIu64" (%u)\n", gpt->header.partition_lba, table_size, (unsigned) table_size >> u);

    uint64_t backup_lba = gpt->header.backup_lba;

    // backup

    gpt = &gpt_list->backup[u - MIN_BLOCK_SHIFT];

    gpt->header.partition_entries = real_entries;
    gpt->table_size = table_size;
    gpt->header.current_lba = backup_lba;
    gpt->header.backup_lba = 1;

    table_end = align_down(table_end, u);

    gpt->header.partition_lba = (table_end - table_size) >> u;

    resize_data(&gpt->header_block, 1 << u);
    resize_data(&gpt->entry_blocks, table_size);

    if(!gpt->header_block.ptr || !gpt->entry_blocks.ptr) {
      fprintf(stderr, "malloc: out of memory\n");
      return 0;
    }

    gpt->header.partition_crc = chksum_crc32(gpt->entry_blocks.ptr, gpt->entry_blocks.len);

    // printf("   back partition_lba = %"PRIu64", back lba = %"PRIu64"\n", gpt->header.partition_lba, gpt->header.current_lba);

    table_ofs += table_size;
    table_end -= table_size;

    gpt_list->primary_end = table_ofs;
    gpt_list->backup_start = table_end;
  }

  uint64_t first_free = table_ofs;
  uint64_t end_free = table_end;

  first_free = align_up(first_free, max_shift);
  end_free = align_down(end_free, max_shift);

  if(opt.align_1m) {
    uint64_t first_free_1mb = align_up(first_free, 20);
    if(gpt_list->start_used >= first_free_1mb) {
      first_free = first_free_1mb;
    }
  }

  if(first_free > gpt_list->start_used || end_free < gpt_list->end_used) {
    if(opt.verbose >= 1) {
      unsigned needed = 0;
      if(first_free > gpt_list->start_used) needed = first_free - gpt_list->start_used;
      if(end_free < gpt_list->end_used) needed = MAX(needed, gpt_list->end_used - end_free);
      printf("%u bytes needed\n", needed);
    }

    fprintf(stderr, "not enough free space for gpt - try option '--entries' to reduce GPT size\n");
    return 0;
  }

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


int write_gpt(disk_t *disk, gpt_t *gpt)
{
  if(!gpt->ok) return 1;

  if(!write_cache(disk, gpt->header.current_lba << gpt->block_shift, &gpt->header_block)) return 0;

  if(!write_cache(disk, gpt->header.partition_lba << gpt->block_shift, &gpt->entry_blocks)) return 0;

  return 1;
}


int write_gpt_list(disk_t *disk, gpt_list_t *gpt_list)
{
  // prepare cache
  disk->cache[0] = (cache_t) {
    .start = 0,
    .data = { .len = gpt_list->primary_end, .ptr = calloc(1, gpt_list->primary_end) }
  };

  disk->cache[1] = (cache_t) {
    .start = gpt_list->backup_start,
    .data = { .len = disk->size - gpt_list->backup_start, .ptr = calloc(1, disk->size - gpt_list->backup_start) }
  };

  if(!disk->cache[0].data.ptr || !disk->cache[1].data.ptr) {
    fprintf(stderr, "malloc: out of memory\n");
    return 0;
  }

  // update pmbr
  if(!write_cache(disk, 0, &gpt_list->pmbr_block)) return 0;

  // primary tables
  for(unsigned u = MIN_BLOCK_SHIFT; u <= MAX_BLOCK_SHIFT; u++) {
    if(!write_gpt(disk, &gpt_list->primary[u - MIN_BLOCK_SHIFT])) return 0;
  }

  // backup tables in reverse order
  for(unsigned u = MAX_BLOCK_SHIFT; u >= MIN_BLOCK_SHIFT; u--) {
    if(!write_gpt(disk, &gpt_list->backup[u - MIN_BLOCK_SHIFT])) return 0;
  }

  // write to disk
  return opt._try ? 1 : flush_cache(disk);
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


uint32_t get_uint32_le(uint8_t *buf)
{
  return ((uint32_t) buf[3] << 24) + (buf[2] << 16) + (buf[1] << 8) + buf[0];
}


uint64_t get_uint64_le(uint8_t *buf)
{
  return ((uint64_t) get_uint32_le(buf + 4) << 32) + get_uint32_le(buf);
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


uint64_t align_down(uint64_t val, unsigned bits)
{
  uint64_t mask = (1u << bits) - 1;

  return val & ~mask;
}


uint64_t align_up(uint64_t val, unsigned bits)
{
  uint64_t mask = (1u << bits) - 1;

  return (val + mask) & ~mask;
}
