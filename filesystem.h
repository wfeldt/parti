typedef struct {
  char *type;
  char *label;
  char *uuid;
} fs_t;

typedef struct file_start_s {
  struct file_start_s *next;
  unsigned block;
  unsigned len;
  char *name;
} file_start_t;

extern fs_t fs;
extern file_start_t *iso_offsets;
extern int iso_read;

int fs_probe(disk_t *disk, uint64_t offset);
int fs_detail_fat(disk_t *disk, int indent, uint64_t sector);
int fs_detail(disk_t *disk, int indent, uint64_t sector);
char *iso_block_to_name(disk_t *disk, unsigned block);
void read_isoinfo(disk_t *disk);
