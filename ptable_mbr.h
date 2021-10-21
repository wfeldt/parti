typedef struct {
  unsigned type;
  unsigned boot:1;
  unsigned valid:1;
  unsigned empty:1;
  struct {
    unsigned c, h, s, lin;
  } start;
  struct {
    unsigned c, h, s, lin;
  } end;
  unsigned real_base;
  unsigned base;
  unsigned idx;
} ptable_t;


unsigned cs2s(unsigned cs);
unsigned cs2c(unsigned cs);
char *mbr_partition_type(unsigned id);
void parse_ptable(void *buf, unsigned addr, ptable_t *ptable, unsigned base, unsigned ext_base, int entries);
int guess_geo(ptable_t *ptable, int entries, unsigned *s, unsigned *h);
void print_ptable_entry(disk_t *disk, int nr, ptable_t *ptable);
int is_ext_ptable(ptable_t *ptable);
ptable_t *find_ext_ptable(ptable_t *ptable, int entries);
void dump_mbr_ptable(disk_t *disk);
