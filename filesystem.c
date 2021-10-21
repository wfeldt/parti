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

fs_t fs;
file_start_t *iso_offsets = NULL;
int iso_read = 0;

int fs_probe(disk_t *disk, uint64_t offset)
{
  const char *data;

  free(fs.type);
  free(fs.label);
  free(fs.uuid);

  memset(&fs, 0, sizeof fs);

  // XXX FIXME
  return 0;

  if(disk->fd) {
    blkid_probe pr = blkid_new_probe();

    // printf("ofs = %llu?\n", (unsigned long long) offset);

    blkid_probe_set_device(pr, disk->fd, offset, 0);

    // blkid_probe_get_value(pr, n, &name, &data, &size)

    if(blkid_do_safeprobe(pr) == 0) {
      if(!blkid_probe_lookup_value(pr, "TYPE", &data, NULL)) {
        fs.type = strdup(data);

        if(!blkid_probe_lookup_value(pr, "LABEL", &data, NULL)) {
          fs.label = strdup(data);
        }

        if(!blkid_probe_lookup_value(pr, "UUID", &data, NULL)) {
          fs.uuid = strdup(data);
        }
      }
    }

    blkid_free_probe(pr);
  }

  // if(fs.type) printf("ofs = %llu, type = '%s', label = '%s', uuid = '%s'\n", (unsigned long long) offset, fs.type, fs.label ?: "", fs.uuid ?: "");

  return fs.type ? 1 : 0;
}


/*
 * Print fat file system details.
 *
 * The fs starts at sector (sector size is disk->block_size).
 * The output is indented by 'indent' spaces.
 * If indent is 0, prints also a separator line.
 */
int fs_detail_fat(disk_t *disk, int indent, uint64_t sector)
{
  unsigned char buf[disk->block_size];
  int i;
  unsigned bpb_len, fat_bits, bpb32;
  unsigned bytes_p_sec, sec_p_cluster, resvd_sec, fats, root_ents, sectors;
  unsigned hidden, fat_secs, data_start, clusters, root_secs;
  unsigned drv_num;

  // XXX FIXME
  return 0;

  if(disk->block_size < 0x200) return 0;

  i = disk_read(disk, buf, sector, 1);

  if(i || read_word_le(buf + 0x1fe) != 0xaa55) return 0;

  if(read_byte(buf) == 0xeb) {
    i = 2 + (int8_t) read_byte(buf + 1);
  }
  else if(read_byte(buf) == 0xe9) {
    i = 3 + (int16_t) read_word_le(buf + 1);
  }
  else {
    i = 0;
  }

  if(i < 3) return 0;

  bpb_len = i;

  if(!strcmp(cname(buf + 3, 8), "NTFS")) return 0;

  bytes_p_sec = read_word_le(buf + 11);
  sec_p_cluster = read_byte(buf + 13);
  resvd_sec = read_word_le(buf + 14);
  fats = read_byte(buf + 16);
  root_ents = read_word_le(buf + 17);
  sectors = read_word_le(buf + 19);
  hidden = read_dword_le(buf + 28);
  if(!sectors) sectors = read_dword_le(buf + 32);
  fat_secs = read_word_le(buf + 22);
  bpb32 = fat_secs ? 0 : 1;
  if(bpb32) fat_secs = read_dword_le(buf + 36);

  if(!sec_p_cluster || !fats) return 0;

  // bytes_p_sec should be a power of 2 and > 0
  if(!bytes_p_sec || (bytes_p_sec & (bytes_p_sec - 1))) return 0;

  root_secs = (root_ents * 32 + bytes_p_sec - 1 ) / bytes_p_sec;

  data_start = resvd_sec + fats * fat_secs + root_secs;

  clusters = (sectors - data_start) / sec_p_cluster;

  fat_bits = 12;
  if(clusters >= 4085) fat_bits = 16;
  if(clusters >= 65525) fat_bits = 32;

  drv_num = read_byte(buf + (bpb32 ? 64 : 36));

  if(indent == 0) printf(SEP "\n");

  printf("%*sfat%u:\n", indent, "", fat_bits);

  indent += 2;

  printf("%*ssector size: %u\n", indent, "", bytes_p_sec);

  printf(
    "%*sbpb[%u], oem \"%s\", media 0x%02x, drive 0x%02x, hs %u/%u\n", indent, "",
    bpb_len,
    cname(buf + 3, 8),
    read_byte(buf + 21),
    drv_num,
    read_word_le(buf + 26),
    read_word_le(buf + 24)
  );

  if(read_byte(buf + (bpb32 ? 66 : 38)) == 0x29) {
    printf("%*svol id 0x%08x, label \"%s\"", indent, "",
      read_dword_le(buf + (bpb32 ? 67 : 39)),
      cname(buf + (bpb32 ? 71 : 43), 11)
    );
    printf(", fs type \"%s\"\n", cname(buf + (bpb32 ? 82 : 54), 8));
  }

  if(bpb32) {
    printf("%*sextflags 0x%02x, fs ver %u.%u, fs info %u, backup bpb %u\n", indent, "",
      read_byte(buf + 40),
      read_byte(buf + 43), read_byte(buf + 42),
      read_word_le(buf + 48),
      read_word_le(buf + 50)
    );
  }

  printf("%*sfs size %u, hidden %u, data start %u\n", indent, "",
    sectors,
    hidden,
    data_start
  );

  printf("%*scluster size %u, clusters %u\n", indent, "",
    sec_p_cluster,
    clusters
  );

  printf("%*sfats %u, fat size %u, fat start %u\n", indent, "",
    fats,
    fat_secs,
    resvd_sec
  );

  if(bpb32) {
    printf("%*sroot cluster %u\n", indent, "",
      read_dword_le(buf + 44)
    );
  }
  else {
    printf("%*sroot entries %u, root size %u, root start %u\n", indent, "",
      root_ents,
      root_secs,
      resvd_sec + fats * fat_secs
    );
  }

  return 1;
}


/*
 * Print file system details.
 *
 * The fs starts at sector (sector size is disk->block_size).
 * The output is indented by 'indent' spaces.
 * If indent is 0, prints also a separator line.
 */
int fs_detail(disk_t *disk, int indent, uint64_t sector)
{
  char *s;
  int fs_ok = fs_probe(disk, sector * disk->block_size);

  if(!fs_ok) return fs_ok;

  if(indent == 0) {
    printf(SEP "\nfile system:\n");
    indent += 2;
  }

  printf("%*sfs \"%s\"", indent, "", fs.type);
  if(fs.label) printf(", label \"%s\"", fs.label);
  if(fs.uuid) printf(", uuid \"%s\"", fs.uuid);

  if((s = iso_block_to_name(disk, (sector * disk->block_size) >> 9))) {
    printf(", \"%s\"", s);
  }
  printf("\n");

  fs_detail_fat(disk, indent, sector);

  return fs_ok;
}


/*
 * block is in 512 byte units
 */
char *iso_block_to_name(disk_t *disk, unsigned block)
{
  static char *buf = NULL;
  file_start_t *fs;
  char *name = NULL;

  if(!iso_read) read_isoinfo(disk);

  for(fs = iso_offsets; fs; fs = fs->next) {
    if(block >= fs->block && block < fs->block + (((fs->len + 2047) >> 11) << 2)) break;
  }

  if(fs) {
    if(block == fs->block) {
      name = fs->name;
    }
    else {
      free(buf);
      asprintf(&buf, "%s<+%u>", fs->name, block - fs->block);
      name = buf;
    }
  }

  return name;
}


void read_isoinfo(disk_t *disk)
{
  FILE *p;
  char *cmd, *s, *t, *line = NULL, *dir = NULL;
  size_t line_len = 0;
  unsigned u1, u2;
  file_start_t *fs;

  iso_read = 1;

  if(!fs_probe(disk, 0)) return;

  asprintf(&cmd, "/usr/bin/isoinfo -R -l -i %s 2>/dev/null", disk->name);

  if((p = popen(cmd, "r"))) {
    while(getline(&line, &line_len, p) != -1) {
      char *line_start = line;

      // isoinfo from mkisofs produces different output than the one from genisoimage
      // remove the optional 1st column (bsc#1097814)
      while(isspace(*line_start) || isdigit(*line_start)) line_start++;

      if(sscanf(line_start, "Directory listing of %m[^\n]", &s) == 1) {
        free(dir);
        dir = s;
      }
      else if(sscanf(line_start, "%*s %*s %*s %*s %u %*[^[][ %u %*u ] %m[^\n]", &u2, &u1, &s) == 3) {
        if(*s) {
          t = s + strlen(s) - 1;
          while(t >= s && isspace(*t)) *t-- = 0;

          if(strcmp(s, ".") && strcmp(s, "..")) {
            fs = calloc(1, sizeof *fs);
            fs->next = iso_offsets;
            iso_offsets = fs;
            fs->block = u1 << 2;
            fs->len = u2;
            asprintf(&fs->name, "%s%s", dir, s);
            free(s);
          }
        }
      }
    }

    pclose(p);
  }

  free(cmd);
  free(dir);

#if 0
  for(fs = iso_offsets; fs; fs = fs->next) {
    printf("block = %u, len = %u, name = '%s'\n", fs->block, fs->len, fs->name);
  }
#endif
}
