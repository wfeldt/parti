#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdarg.h>
#include <getopt.h>
#include <inttypes.h>
#include <ctype.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <linux/fs.h>	/* BLKGETSIZE64 */
#include <x86emu.h>


#define FIRST_DISK	0x80
#define FIRST_CDROM	0xe0
#define MAX_DISKS	0x100

#include "uni.h"

typedef struct {
  unsigned char scan;
  unsigned char ascii;
  char *name;
} bios_key_t;

#include "bios_keys.h"

typedef struct {
  unsigned type;
  unsigned boot:1;
  unsigned valid:1;
  struct {
    unsigned c, h, s, lin;
  } start;
  struct {
    unsigned c, h, s, lin;
  } end;
  unsigned base;
} ptable_t;


typedef struct {
  x86emu_t *emu;

  unsigned kbd_cnt;
  unsigned key;

  unsigned memsize;	// in MB

  unsigned a20:1;
  unsigned maybe_linux_kernel:1;
  unsigned is_linux_kernel:1;

  struct {
    char version[0x100];
    char cmdline[0x1000];
    unsigned loader_id;
    unsigned boot_proto;
    unsigned code_start;
    unsigned initrd_start;
    unsigned initrd_length;
    unsigned cmdline_start;
  } kernel;

  struct {
    unsigned iv_base;
    int (* iv_funcs[0x100])(x86emu_t *emu);
  } bios;
} vm_t;


void lprintf(const char *format, ...) __attribute__ ((format (printf, 1, 2)));
void flush_log(x86emu_t *emu, char *buf, unsigned size);

void help(void);
uint64_t vm_read_qword(x86emu_t *emu, unsigned addr);
unsigned vm_read_segofs16(x86emu_t *emu, unsigned addr);
void vm_write_qword(x86emu_t *emu, unsigned addr, uint64_t val);
void handle_int(x86emu_t *emu, unsigned nr);
int check_ip(x86emu_t *emu);
vm_t *vm_new(void);
void vm_free(vm_t *vm);
void vm_run(vm_t *vm);
unsigned cs2s(unsigned cs);
unsigned cs2c(unsigned cs);
int do_int(x86emu_t *emu, u8 num, unsigned type);
int do_int_10(x86emu_t *emu);
int do_int_11(x86emu_t *emu);
int do_int_12(x86emu_t *emu);
int do_int_13(x86emu_t *emu);
int do_int_15(x86emu_t *emu);
int do_int_16(x86emu_t *emu);
int do_int_19(x86emu_t *emu);
int do_int_1a(x86emu_t *emu);
void prepare_bios(vm_t *vm);
void prepare_boot(x86emu_t *emu);
int disk_read(x86emu_t *emu, unsigned addr, unsigned disk, uint64_t sector, unsigned cnt, int log);
int disk_write(x86emu_t *emu, unsigned addr, unsigned disk, uint64_t sector, unsigned cnt, int log);
void parse_ptable(x86emu_t *emu, unsigned addr, ptable_t *ptable, unsigned base, unsigned ext_base, int entries);
int guess_geo(ptable_t *ptable, int entries, unsigned *s, unsigned *h);
void print_ptable_entry(int nr, ptable_t *ptable);
int is_ext_ptable(ptable_t *ptable);
ptable_t *find_ext_ptable(ptable_t *ptable, int entries);
void dump_ptable(x86emu_t *emu, unsigned disk);
char *get_screen(x86emu_t *emu);
void dump_screen(x86emu_t *emu);
unsigned next_bios_key(char **keys);


struct option options[] = {
  { "help",       0, NULL, 'h'  },
  { "verbose",    0, NULL, 'v'  },
  { "disk",       1, NULL, 1001 },
  { "floppy",     1, NULL, 1002 },
  { "cdrom",      1, NULL, 1003 },
  { "boot",       1, NULL, 1004 },
  { "show",       1, NULL, 1005 },
  { "feature",    1, NULL, 1007 },
  { "no-feature", 1, NULL, 1008 },
  { "max",        1, NULL, 1009 },
  { "log-size",   1, NULL, 1010 },
  { "keys",       1, NULL, 1011 },
  { }
};

struct {
  unsigned verbose;
  unsigned inst_max;
  unsigned log_size; 

  struct {
    char *dev;
    int fd;
    unsigned heads;
    unsigned sectors;
    unsigned cylinders;
    uint64_t size;
  } disk[MAX_DISKS];

  unsigned floppies;
  unsigned disks;
  unsigned cdroms;
  unsigned boot;

  unsigned trace_flags;
  unsigned dump_flags;

  struct {
    unsigned rawptable:1;
  } show;

  struct {
    unsigned edd:1;
  } feature;

  FILE *log_file;
  char *keyboard;
} opt;


int main(int argc, char **argv)
{
  char *s, *t, *dev_spec, *err_msg = NULL;
  int i, j, err;
  unsigned u, u2, ofs, *uu, tbits, dbits;
  struct stat sbuf;
  uint64_t ul;
  ptable_t ptable[4];
  x86emu_t *emu_0 = x86emu_new(X86EMU_PERM_R | X86EMU_PERM_W, 0);
  vm_t *vm;

  opt.log_file = stdout;

  opt.inst_max = 10000000;
  opt.feature.edd = 1;

  opterr = 0;

  while((i = getopt_long(argc, argv, "hv", options, NULL)) != -1) {
    err = 0;
    dev_spec = NULL;

    switch(i) {
      case 'v':
        opt.verbose++;
        break;

      case 1001:
      case 1002:
      case 1003:
        if(i == 1001) {
          if(opt.disks >= FIRST_CDROM - FIRST_DISK) break;
          uu = &opt.disks;
          ofs = FIRST_DISK + opt.disks;
        }
        else if(i == 1002) {
          if(opt.floppies >= FIRST_DISK) break;
          uu = &opt.floppies;
          ofs = opt.floppies;
        }
        else /* i == 1003 */ {
          if(opt.cdroms >= MAX_DISKS - FIRST_CDROM) break;
          uu = &opt.cdroms;
          ofs = FIRST_CDROM + opt.cdroms;
        }

        dev_spec = strdup(optarg);
        if((s = strchr(dev_spec, ','))) {
          *s++ = 0;
        }
        if(!*dev_spec) {
          err = 1;
          break;
        }
        opt.disk[ofs].dev = strdup(dev_spec);
        if(s) {
          u = strtoul(s, &t, 0);
          if((*t == 0 || *t == ',') && u <= 255) {
            opt.disk[ofs].heads = u;
          }
          else {
            err = 2;
            break;
          }
          if(*t++ == ',') {
            u = strtoul(t, &t, 0);
            if(*t == 0 && u <= 63) {
              opt.disk[ofs].sectors = u;
            }
            else {
              err = 3;
              break;
            }
          }
        }
        (*uu)++;
        break;

      case 1004:
        if(!strcmp(optarg, "floppy")) {
          opt.boot = 0;
        }
        else if(!strcmp(optarg, "disk")) {
          opt.boot = FIRST_DISK;
        }
        else if(!strcmp(optarg, "cdrom")) {
          opt.boot = FIRST_CDROM;
        }
        else {
          u = strtoul(optarg, &s, 0);
          if(s != optarg && !*s && u < MAX_DISKS) {
            opt.boot = u;
          }
          else {
            err = 4;
          }
        }
        break;

      case 1005:
        for(s = optarg; (t = strsep(&s, ",")); ) {
          u = 1;
          tbits = dbits = 0;
          while(*t == '+' || *t == '-') u = *t++ == '+' ? 1 : 0;
               if(!strcmp(t, "trace")) tbits = X86EMU_TRACE_DEFAULT;
          else if(!strcmp(t, "code"))  tbits = X86EMU_TRACE_CODE;
          else if(!strcmp(t, "regs"))  tbits = X86EMU_TRACE_REGS;
          else if(!strcmp(t, "data"))  tbits = X86EMU_TRACE_DATA;
          else if(!strcmp(t, "acc"))   tbits = X86EMU_TRACE_ACC;
          else if(!strcmp(t, "io"))    tbits = X86EMU_TRACE_IO;
          else if(!strcmp(t, "ints"))  tbits = X86EMU_TRACE_INTS;
          else if(!strcmp(t, "time"))  tbits = X86EMU_TRACE_TIME;
          else if(!strcmp(t, "debug")) tbits = X86EMU_TRACE_DEBUG;
          else if(!strcmp(t, "dump"))         dbits = X86EMU_DUMP_DEFAULT;
          else if(!strcmp(t, "dump.regs"))    dbits = X86EMU_DUMP_REGS;
          else if(!strcmp(t, "dump.mem"))     dbits = X86EMU_DUMP_MEM;
          else if(!strcmp(t, "dump.mem.acc")) dbits = X86EMU_DUMP_ACC_MEM;
          else if(!strcmp(t, "dump.mem.inv")) dbits = X86EMU_DUMP_INV_MEM;
          else if(!strcmp(t, "dump.attr"))    dbits = X86EMU_DUMP_ATTR;
          else if(!strcmp(t, "dump.io"))      dbits = X86EMU_DUMP_IO;
          else if(!strcmp(t, "dump.ints"))    dbits = X86EMU_DUMP_INTS;
          else if(!strcmp(t, "dump.time"))    dbits = X86EMU_DUMP_TIME;
          else if(!strcmp(t, "rawptable"))    opt.show.rawptable = u;
          else err = 5;
          if(err) {
            err_msg = t;
          }
          else {
            if(tbits) {
              if(u) {
                opt.trace_flags |= tbits;
              }
              else {
                opt.trace_flags &= ~tbits;
              }
            }
            if(dbits) {
              if(u) {
                opt.dump_flags |= dbits;
              }
              else {
                opt.dump_flags &= ~dbits;
              }
            }
          }
        }
        break;

      case 1007:
      case 1008:
        s = optarg;
        u = i == 1007 ? 1 : 0;
        while((t = strsep(&s, ","))) {
          if(!strcmp(t, "edd")) opt.feature.edd = u;
          else err = 6;
        }
        break;

      case 1009:
        opt.inst_max = strtoul(optarg, NULL, 0);
        break;

      case 1010:
        opt.log_size = strtoul(optarg, NULL, 0);
        break;

      case 1011:
        opt.keyboard = optarg;
        break;

      default:
        help();
        return i == 'h' ? 0 : 1;
    }

    free(dev_spec);

    if(err && (i == 1001 || i == 1002 || i == 1003)) {
      fprintf(stderr, "invalid device spec: %s\n", optarg);
      return 1;
    }

    if(err && i == 1004) {
      fprintf(stderr, "invalid boot device: %s\n", optarg);
      return 1;
    }

    if(err && (i == 1005 || i == 1006)) {
      fprintf(stderr, "invalid show spec: %s\n", err_msg);
      return 1;
    }

    if(err && (i == 1007 || i == 1008)) {
      fprintf(stderr, "invalid feature: %s\n", optarg);
      return 1;
    }
  }

  if(!opt.disks && !opt.floppies && !opt.cdroms) {
    fprintf(stderr, "we need some drives\n");
    return 1;
  }

  if(!opt.disk[opt.boot].dev) {
    if(opt.disk[FIRST_CDROM].dev) {
      opt.boot = FIRST_CDROM;
    }
    else if(opt.disk[FIRST_DISK].dev) {
      opt.boot = FIRST_DISK;
    }
    else if(opt.disk[0].dev) {
      opt.boot = 0;
    }
  }

  lprintf("; drive map:\n");

  for(i = 0; i < MAX_DISKS; i++) {
    opt.disk[i].fd = -1;
    if(!opt.disk[i].dev) continue;

    opt.disk[i].fd = open(opt.disk[i].dev, O_RDONLY | O_LARGEFILE);
    if(opt.disk[i].fd < 0) continue;

    if(!opt.disk[i].heads || !opt.disk[i].sectors) {
      j = disk_read(emu_0, 0, i, 0, 1, 0);
      if(!j && x86emu_read_word(emu_0, 0x1fe) == 0xaa55) {
        parse_ptable(emu_0, 0x1be, ptable, 0, 0, 4);
        if(guess_geo(ptable, 4, &u, &u2)) {
          if(!opt.disk[i].sectors) opt.disk[i].sectors = u;
          if(!opt.disk[i].heads) opt.disk[i].heads = u2;
        }
      }
    }

    if(!opt.disk[i].heads) opt.disk[i].heads = 255;
    if(!opt.disk[i].sectors) opt.disk[i].sectors = 63;

    ul = 0;
    if(!fstat(opt.disk[i].fd, &sbuf)) ul = sbuf.st_size;
    if(!ul && ioctl(opt.disk[i].fd, BLKGETSIZE64, &ul)) ul = 0;
    opt.disk[i].size = ul >> 9;
    opt.disk[i].cylinders = opt.disk[i].size / (opt.disk[i].sectors * opt.disk[i].heads);

    lprintf(";   0x%02x: %s, chs %u/%u/%u, %llu sectors\n",
      i,
      opt.disk[i].dev,
      opt.disk[i].cylinders,
      opt.disk[i].heads,
      opt.disk[i].sectors,
      (unsigned long long) opt.disk[i].size
     );

    dump_ptable(emu_0, i);
  }

  emu_0 = x86emu_done(emu_0);

  lprintf("; boot device: 0x%02x\n", opt.boot);

  fflush(stdout);

  vm = vm_new();

  prepare_bios(vm);

  prepare_boot(vm->emu);

  vm_run(vm);

  dump_screen(vm->emu);

  vm_free(vm);

  for(u = 0; u < MAX_DISKS; u++) {
    free(opt.disk[u].dev);
  }

  return 0;
}


void lprintf(const char *format, ...)
{
  va_list args;

  va_start(args, format);
  if(opt.log_file) vfprintf(opt.log_file, format, args);
  va_end(args);
}


void flush_log(x86emu_t *emu, char *buf, unsigned size)
{
  if(!buf || !size || !opt.log_file) return;

  fwrite(buf, size, 1, opt.log_file);
}


void help()
{
  fprintf(stderr,
    "Boot Loader Test\nUsage: bloat OPTIONS\n"
    "\n"
    "Options:\n"
    "  --boot DEVICE\n"
    "      boot from DEVICE\n"
    "      DEVICE is either a number (0-0xff) or one of: floppy, disk, cdrom\n"
    "  --disk device[,heads,sectors]\n"
    "      add hard disk image [with geometry]\n"
    "  --floppy device[,heads,sectors]\n"
    "      add floppy disk image [with geometry]\n"
    "  --cdrom device[,heads,sectors]\n"
    "      add cdrom image [with geometry]\n"
    "  --show LIST\n"
    "      Things to log. LIST is a comma-separated list of code, regs, data, acc,\n"
    "      io, ints, time, debug, dump.regs, dump.mem, dump.mem.acc, dump.mem.inv,\n"
    "      dump.attr, dump.io, dump.ints, dump.time.\n"
    "      Every item can be prefixed by '-' to turn it off.\n"
    "      Use trace and dump as shorthands for a useful combination of items\n"
    "      from the above list.\n"
    "  --feature LIST\n"
    "      features to enable\n"
    "      LIST is a comma-separated list of: edd\n"
    "  --no-feature LIST\n"
    "      features to disable (see --features)\n"
    "  --max N\n"
    "      stop after N instructions\n"
    "  --key STRING\n"
    "      use STRING as keyboard input\n"
    "  --log-size N\n"
    "      internal log buffer size\n"
    "  --help\n"
    "      show this text\n"
    "examples:\n"
    "  bloat --floppy floppy.img --disk /dev/sda --disk foo.img --boot floppy\n"
    "  bloat --disk linux.img\n"
    "  bloat --cdrom foo.iso --show code,regs\n"
  );
}


uint64_t vm_read_qword(x86emu_t *emu, unsigned addr)
{
  return x86emu_read_dword(emu, addr) + ((uint64_t) x86emu_read_dword(emu, addr + 4) << 32);
}


unsigned vm_read_segofs16(x86emu_t *emu, unsigned addr)
{
  return x86emu_read_word(emu, addr) + (x86emu_read_word(emu, addr + 2) << 4);
}


void vm_write_qword(x86emu_t *emu, unsigned addr, uint64_t val)
{
  x86emu_write_dword(emu, addr, val);
  x86emu_write_dword(emu, addr + 4, val >> 32);
}


unsigned cs2s(unsigned cs)
{
  return cs & 0x3f;
}


unsigned cs2c(unsigned cs)
{
  return ((cs >> 8) & 0xff) + ((cs & 0xc0) << 2);
}


int check_ip(x86emu_t *emu)
{
  vm_t *vm = emu->private;
  unsigned eip, u1, v;

  eip = emu->x86.R_CS_BASE + emu->x86.R_EIP;

  if(eip >= vm->bios.iv_base && eip < vm->bios.iv_base + 0x100) {
    handle_int(emu, eip - vm->bios.iv_base);

    return 0;
  }

  if(vm->maybe_linux_kernel) {
    if(
      emu->x86.R_EIP < 0x1000 &&
      x86emu_read_word(emu, eip) == 0x8166 &&		// cmp dword [????],0x5a5aaa55
      x86emu_read_dword(emu, eip + 5) == 0x5a5aaa55 &&
      x86emu_read_dword(emu, emu->x86.R_CS_BASE + 0x202) == 0x53726448	// "HdrS"
    ) {
      // very likely we are starting a linux kernel
      vm->is_linux_kernel = 1;

      vm->kernel.loader_id = x86emu_read_byte(emu, emu->x86.R_CS_BASE + 0x210);
      vm->kernel.boot_proto = x86emu_read_word(emu, emu->x86.R_CS_BASE + 0x206);

      vm->kernel.code_start = x86emu_read_dword(emu, emu->x86.R_CS_BASE + 0x214);
      vm->kernel.initrd_start = x86emu_read_dword(emu, emu->x86.R_CS_BASE + 0x218);
      vm->kernel.initrd_length = x86emu_read_dword(emu, emu->x86.R_CS_BASE + 0x21c);

      vm->kernel.cmdline_start = x86emu_read_dword(emu, emu->x86.R_CS_BASE + 0x228);

      u1 = emu->x86.R_CS_BASE + x86emu_read_word(emu, emu->x86.R_CS_BASE + 0x20e) + 0x200;
      for(v = 0; v < sizeof vm->kernel.version - 1; v++) {
        if(!(vm->kernel.version[v] = x86emu_read_byte(emu, u1 + v))) break;
      }
      vm->kernel.version[v] = 0;

      for(v = 0; v < sizeof vm->kernel.cmdline - 1; v++) {
        if(!(vm->kernel.cmdline[v] = x86emu_read_byte(emu, vm->kernel.cmdline_start + v))) break;
      }
      vm->kernel.cmdline[v] = 0;

      x86emu_log(emu, "; linux kernel version = \"%s\"\n", vm->kernel.version);
      x86emu_log(emu, ";   boot proto = %u.%u, loader id = 0x%02x\n",
        vm->kernel.boot_proto >> 8, vm->kernel.boot_proto & 0xff,
        vm->kernel.loader_id
      );
      x86emu_log(emu, ";   code start = 0x%08x, initrd start = 0x%08x, length = 0x%08x\n",
        vm->kernel.code_start,
        vm->kernel.initrd_start, vm->kernel.initrd_length
      );
      x86emu_log(emu, ";   cmdline start = 0x%08x\n", vm->kernel.cmdline_start);
      x86emu_log(emu, ";   cmdline = \"%s\"\n", vm->kernel.cmdline);
      x86emu_log(emu, "# bzImage kernel start detected -- stopping\n");

      return 1;
    }
  }

  vm->maybe_linux_kernel = 0;

  if(
    emu->x86.R_EIP < 0x1000 &&
    x86emu_read_word(emu, eip) == 0x1c75 &&		// jnz gfx_init_10
    x86emu_read_dword(emu, eip + 2) == 0xf50073f5		// cmc ; jnc $ + 2 ; cmc
  ) {
    x86emu_log(emu, "# gfxboot init detected -- aborting\n");
    emu->x86.R_EIP += 2;
  }
  else if(
    emu->x86.R_EIP < 0x200 &&
    x86emu_read_byte(emu, eip) == 0xcb &&			// retf
    x86emu_read_word(emu, eip + 1) == 0x8166 &&		// cmp dword [????],0x5a5aaa55
    x86emu_read_dword(emu, eip + 6) == 0x5a5aaa55
  ) {
    vm->maybe_linux_kernel = 1;
  }
  else if(
    emu->x86.R_EIP == 0 &&
    eip == 0x90200 &&
    x86emu_read_dword(emu, 0x90000) == 0x8e07c0b8 &&
    x86emu_read_dword(emu, 0x90004) == 0x9000b8d8
  ) {
    vm->is_linux_kernel = 1;

    x86emu_log(emu, "# zImage kernel start detected -- stopping\n");

    return 1;
  }

  return 0;
}


void handle_int(x86emu_t *emu, unsigned nr)
{
  vm_t *vm = emu->private;
  int stop = 0;
  u8 flags;

  if(!vm->bios.iv_funcs[nr]) {
    x86emu_log(emu, "# unhandled interrupt 0x%02x\n", nr);
    stop = 1;
  }
  else {
    stop = vm->bios.iv_funcs[nr](emu);
    flags = emu->x86.R_FLG;
    x86emu_write_byte(emu, emu->x86.R_SS_BASE + ((emu->x86.R_SP + 4) & 0xffff), flags);
  }

  if(stop) x86emu_stop(emu);
}


int do_int(x86emu_t *emu, u8 num, unsigned type)
{
  if((type & 0xff) == INTR_TYPE_FAULT) {
    x86emu_stop(emu);

    return 0;
  }

  if(x86emu_read_word(emu, num * 4)) return 0;

  x86emu_log(emu, "# unhandled interrupt 0x%02x\n", num);

  return 1;
}


int do_int_10(x86emu_t *emu)
{
  unsigned u, cnt, attr;
  unsigned cur_x, cur_y, page;
  unsigned x, y, x0, y0, x1, y1, width, d;

  switch(emu->x86.R_AH) {
    case 0x01:
      x86emu_log(emu, "; int 0x10: ah 0x%02x (set cursor shape)\n", emu->x86.R_AH);
      // emu->x86.R_CX: shape
      break;

    case 0x02:
      x86emu_log(emu, "; int 0x10: ah 0x%02x (set cursor)\n", emu->x86.R_AH);
      x86emu_log(emu, "; (x, y) = (%u, %u)\n", emu->x86.R_DL, emu->x86.R_DH);
      page = emu->x86.R_BH & 7;
      x86emu_write_byte(emu, 0x450 + 2 * page, emu->x86.R_DL);	// x
      x86emu_write_byte(emu, 0x451 + 2 * page, emu->x86.R_DH);	// y
      break;

    case 0x03:
      x86emu_log(emu, "; int 0x10: ah 0x%02x (get cursor)\n", emu->x86.R_AH);
      page = emu->x86.R_BH & 7;
      emu->x86.R_DL = x86emu_read_byte(emu, 0x450 + 2 * page);	// x
      emu->x86.R_DH = x86emu_read_byte(emu, 0x451 + 2 * page);	// y
      emu->x86.R_CX = 0x607;					// cursor shape
      x86emu_log(emu, "; (x, y) = (%u, %u)\n", emu->x86.R_DL, emu->x86.R_DH);
      break;

    case 0x06:
      x86emu_log(emu, "; int 0x10: ah 0x%02x (scroll up)\n", emu->x86.R_AH);
      attr = 0x20 + (emu->x86.R_BH << 8);
      x0 = emu->x86.R_CL;
      y0 = emu->x86.R_CH;
      x1 = emu->x86.R_DL;
      y1 = emu->x86.R_DH;
      d = emu->x86.R_AL;
      x86emu_log(emu, ";   window (%u, %u) - (%u, %u), by %u lines\n", x0, y0, x1, y1, d);
      width = x86emu_read_byte(emu, 0x44a);
      if(x0 > width) x0 = width;
      if(x1 > width) x1 = width;
      u = x86emu_read_byte(emu, 0x484);
      if(y0 > u) y0 = u;
      if(y1 > u) y1 = u;
      if(y1 > y0 && x1 > x0) {
        if(d == 0) {
          for(y = y0; y <= y1; y++) {
            for(x = x0; x < x1; x++) {
              x86emu_write_word(emu, 0xb8000 + 2 * (x + width * y), attr);
            }
          }
        }
        else {
          for(y = y0; y < y1; y++) {
            for(x = x0; x < x1; x++) {
              u = x86emu_read_word(emu, 0xb8000 + 2 * (x + width * (y + 1)));
              x86emu_write_word(emu, 0xb8000 + 2 * (x + width * y), u);
            }
          }
          for(x = x0; x < x1; x++) {
            x86emu_write_word(emu, 0xb8000 + 2 * (x + width * y), attr);
          }
        }
      }
      break;

    case 0x09:
      x86emu_log(emu, "; int 0x10: ah 0x%02x (write char & attr)\n", emu->x86.R_AH);
      u = emu->x86.R_AL;
      attr = emu->x86.R_BL;
      page = emu->x86.R_BH & 7;
      cnt = emu->x86.R_CX;
      cur_x = x86emu_read_byte(emu, 0x450 + 2 * page);
      cur_y = x86emu_read_byte(emu, 0x451 + 2 * page);
      x86emu_log(emu, "; char 0x%02x '%c', attr 0x%02x, cnt %u\n", u, u >= 0x20 && u < 0x7f ? u : ' ', attr, cnt);
      while(cnt--) {
        x86emu_write_byte(emu, 0xb8000 + 2 * (cur_x + 80 * cur_y), u);
        x86emu_write_byte(emu, 0xb8001 + 2 * (cur_x + 80 * cur_y), attr);
        cur_x++;
      }
      break;

    case 0x0e:
      x86emu_log(emu, "; int 0x10: ah 0x%02x (tty print)\n", emu->x86.R_AH);
      u = emu->x86.R_AL;
      page = emu->x86.R_BH & 7;
      cur_x = x86emu_read_byte(emu, 0x450 + 2 * page);
      cur_y = x86emu_read_byte(emu, 0x451 + 2 * page);
      x86emu_log(emu, "; char 0x%02x '%c'\n", u, u >= 0x20 && u < 0x7f ? u : ' ');
      if(u == 0x0d) {
        cur_x = 0;
      }
      else if(u == 0x0a) {
        cur_y++;
      }
      else {
        x86emu_write_byte(emu, 0xb8000 + 2 * (cur_x + 80 * cur_y), u);
        x86emu_write_byte(emu, 0xb8001 + 2 * (cur_x + 80 * cur_y), 7);
        cur_x++;
        if(cur_x == 80) {
          cur_x = 0;
          cur_y++;
        }
      }
      x86emu_write_byte(emu, 0x450 + 2 * page, cur_x);
      x86emu_write_byte(emu, 0x451 + 2 * page, cur_y);
      break;

    case 0x0f:
      emu->x86.R_AL = x86emu_read_byte(emu, 0x449);	// vide mode
      emu->x86.R_AH = x86emu_read_byte(emu, 0x44a);	// screen width
      emu->x86.R_BH = 0;				// active page
      break;

    default:
      x86emu_log(emu, "; int 0x10: ah 0x%02x\n", emu->x86.R_AH);
      break;
  }

  return 0;
}


int do_int_11(x86emu_t *emu)
{
  x86emu_log(emu, "; int 0x11: (get equipment list)\n");
  emu->x86.R_AX = 0x4026;
  x86emu_log(emu, "; eq mask: %04x\n", emu->x86.R_AX);

  return 0;
}


int do_int_12(x86emu_t *emu)
{
  x86emu_log(emu, "; int 0x12: (get base mem size)\n");
  emu->x86.R_AX = x86emu_read_word(emu, 0x413);
  x86emu_log(emu, "; base mem size: %u kB\n", emu->x86.R_AX);

  return 0;
}


int do_int_13(x86emu_t *emu)
{
  unsigned u, disk, cnt, sector, cylinder, head, addr;
  uint64_t ul;
  int i, j;

  switch(emu->x86.R_AH) {
    case 0x00:
      x86emu_log(emu, "; int 0x13: ah 0x%02x (disk reset)\n", emu->x86.R_AH);
      disk = emu->x86.R_DL;
      x86emu_log(emu, "; drive 0x%02x\n", disk);
      if(disk >= MAX_DISKS || !opt.disk[disk].dev) {
        emu->x86.R_AH = 7;
        X86EMU_SET_FLAG(emu, F_CF);
      }
      else {
        emu->x86.R_AH = 0;
        X86EMU_CLEAR_FLAG(emu, F_CF);
      }
      break;

    case 0x02:
      x86emu_log(emu, "; int 0x13: ah 0x%02x (disk read)\n", emu->x86.R_AH);
      disk = emu->x86.R_DL;
      cnt = emu->x86.R_AL;
      head = emu->x86.R_DH;
      cylinder = cs2c(emu->x86.R_CX);
      sector = cs2s(emu->x86.R_CX);
      addr = (emu->x86.R_ES << 4) + emu->x86.R_BX;
      x86emu_log(emu, "; drive 0x%02x, chs %u/%u/%u, %u sectors, buf 0x%05x\n",
        disk,
        cylinder, head, sector,
        cnt,
        addr
      );
      if(cnt) {
        if(!sector) {
          emu->x86.R_AH = 0x04;
          X86EMU_SET_FLAG(emu, F_CF);
          break;
        }
        ul = (cylinder * opt.disk[disk].heads + head) * opt.disk[disk].sectors + sector - 1;
        i = disk_read(emu, addr, disk, ul, cnt, 1);
        if(i) {
          emu->x86.R_AH = 0x04;
          X86EMU_SET_FLAG(emu, F_CF);
          break;
        }
      }      
      emu->x86.R_AH = 0;
      X86EMU_CLEAR_FLAG(emu, F_CF);
      break;

    case 0x08:
      x86emu_log(emu, "; int 0x13: ah 0x%02x (get drive params)\n", emu->x86.R_AH);
      disk = emu->x86.R_DL;
      x86emu_log(emu, "; drive 0x%02x\n", disk);
      if(
        disk >= MAX_DISKS ||
        !opt.disk[disk].dev ||
        !opt.disk[disk].sectors ||
        !opt.disk[disk].heads
      ) {
        emu->x86.R_AH = 0x07;
        X86EMU_SET_FLAG(emu, F_CF);
        break;
      }
      X86EMU_CLEAR_FLAG(emu, F_CF);
      emu->x86.R_AH = 0;
      emu->x86.R_ES = 0;
      emu->x86.R_DI = 0;
      emu->x86.R_BL = 0;
      emu->x86.R_DL = disk < 0x80 ? opt.floppies : opt.disks;
      emu->x86.R_DH = opt.disk[disk].heads - 1;
      u = opt.disk[disk].cylinders;
      if(u > 1023) u = 1023;
      emu->x86.R_CX = ((u >> 8) << 6) + ((u & 0xff) << 8) + opt.disk[disk].sectors;
      break;

    case 0x15:
      x86emu_log(emu, "; int 0x13: ah 0x%02x (get disk type)\n", emu->x86.R_AH);
      disk = emu->x86.R_DL;
      x86emu_log(emu, "; drive 0x%02x\n", disk);
      X86EMU_CLEAR_FLAG(emu, F_CF);
      if(
        disk >= MAX_DISKS ||
        !opt.disk[disk].dev ||
        !opt.disk[disk].sectors ||
        !opt.disk[disk].heads
      ) {
        emu->x86.R_AH = 0x00;
      }
      else {
        emu->x86.R_AH = 3;
        emu->x86.R_DX = opt.disk[disk].sectors;
        emu->x86.R_CX = opt.disk[disk].sectors >> 16;
      }
      break;

    case 0x41:
      x86emu_log(emu, "; int 0x13: ah 0x%02x (edd install check)\n", emu->x86.R_AH);
      disk = emu->x86.R_DL;
      x86emu_log(emu, "; drive 0x%02x\n", disk);
      if(!opt.feature.edd || disk >= MAX_DISKS || !opt.disk[disk].dev || emu->x86.R_BX != 0x55aa) {
        emu->x86.R_AH = 1;
        X86EMU_SET_FLAG(emu, F_CF);
      }
      else {
        emu->x86.R_AX = 0x3000;
        emu->x86.R_BX = 0xaa55;
        emu->x86.R_CX = 0x000f;
        X86EMU_CLEAR_FLAG(emu, F_CF);
      }
      break;

    case 0x42:
      x86emu_log(emu, "; int 0x13: ah 0x%02x (edd disk read)\n", emu->x86.R_AH);
      disk = emu->x86.R_DL;
      addr = (emu->x86.R_DS << 4) + emu->x86.R_SI;
      x86emu_log(emu, "; drive 0x%02x, request packet:\n; 0x%05x:", disk, addr);
      j = x86emu_read_byte(emu, addr);
      j = j == 0x10 || j == 0x18 ? j : 0x10;
      for(i = 0; i < j; i++) x86emu_log(emu, " %02x", x86emu_read_byte(emu, addr + i));
      x86emu_log(emu, "\n");
      if(
        !opt.feature.edd || disk >= MAX_DISKS || !opt.disk[disk].dev ||
        (x86emu_read_byte(emu, addr) != 0x10 && x86emu_read_byte(emu, addr) != 0x18)
      ) {
        emu->x86.R_AH = 1;
        X86EMU_SET_FLAG(emu, F_CF);
        break;
      }
      cnt = x86emu_read_word(emu, addr + 2);
      u = x86emu_read_dword(emu, addr + 4);
      ul = vm_read_qword(emu, addr + 8);
      if(x86emu_read_byte(emu, addr) == 0x18 && u == 0xffffffff) {
        u = x86emu_read_dword(emu, addr + 0x10);
      }
      else {
        u = vm_read_segofs16(emu, addr + 4);
      }
      if(disk >= FIRST_CDROM) {
        ul <<= 2;
        cnt <<= 2;
      }
      i = disk_read(emu, u, disk, ul, cnt, 1);
      if(i) {
        emu->x86.R_AH = 0x04;
        X86EMU_SET_FLAG(emu, F_CF);
        break;
      }
      emu->x86.R_AH = 0;
      X86EMU_CLEAR_FLAG(emu, F_CF);
      break;

    case 0x43:
      x86emu_log(emu, "; int 0x13: ah 0x%02x (edd disk write)\n", emu->x86.R_AH);
      disk = emu->x86.R_DL;
      addr = (emu->x86.R_DS << 4) + emu->x86.R_SI;
      x86emu_log(emu, "; drive 0x%02x, request packet:\n; 0x%05x:", disk, addr);
      j = x86emu_read_byte(emu, addr);
      j = j == 0x10 || j == 0x18 ? j : 0x10;
      for(i = 0; i < j; i++) x86emu_log(emu, " %02x", x86emu_read_byte(emu, addr + i));
      x86emu_log(emu, "\n");
      if(
        !opt.feature.edd || disk >= MAX_DISKS || !opt.disk[disk].dev ||
        (x86emu_read_byte(emu, addr) != 0x10 && x86emu_read_byte(emu, addr) != 0x18)
      ) {
        emu->x86.R_AH = 1;
        X86EMU_SET_FLAG(emu, F_CF);
        break;
      }
      cnt = x86emu_read_word(emu, addr + 2);
      u = x86emu_read_dword(emu, addr + 4);
      ul = vm_read_qword(emu, addr + 8);
      if(x86emu_read_byte(emu, addr) == 0x18 && u == 0xffffffff) {
        u = x86emu_read_dword(emu, addr + 0x10);
      }
      else {
        u = vm_read_segofs16(emu, addr + 4);
      }
      if(disk >= FIRST_CDROM) {		/* sector size 2k */
        ul <<= 2;
        cnt <<= 2;
      }
      i = disk_write(emu, u, disk, ul, cnt, 1);
      if(i) {
        emu->x86.R_AH = 0x04;
        X86EMU_SET_FLAG(emu, F_CF);
        break;
      }
      emu->x86.R_AH = 0;
      X86EMU_CLEAR_FLAG(emu, F_CF);
      break;

    case 0x48:
      x86emu_log(emu, "; int 0x13: ax 0x%02x (get drive params)\n", emu->x86.R_AH);
      disk = emu->x86.R_DL;
      x86emu_log(emu, "; drive 0x%02x\n", disk);
      if(
        disk >= MAX_DISKS ||
        !opt.disk[disk].dev ||
        !opt.disk[disk].sectors ||
        !opt.disk[disk].heads
      ) {
        emu->x86.R_AH = 0x07;
        X86EMU_SET_FLAG(emu, F_CF);
        break;
      }
      X86EMU_CLEAR_FLAG(emu, F_CF);
      emu->x86.R_AH = 0;

      u = emu->x86.R_DS_BASE + emu->x86.R_SI;

      x86emu_write_word(emu, u, 0x1a);	// buffer size
      x86emu_write_word(emu, u + 2, 3);
      x86emu_write_dword(emu, u + 4, opt.disk[disk].cylinders);
      x86emu_write_dword(emu, u + 8, opt.disk[disk].heads);
      x86emu_write_dword(emu, u + 0xc, opt.disk[disk].sectors);
      vm_write_qword(emu, u + 0x10, opt.disk[disk].size);
      x86emu_write_word(emu, u + 0x18, 0x200);	// sector size
      break;

    case 0x4b:
      x86emu_log(emu, "; int 0x13: ax 0x%04x (terminate disk emulation)\n", emu->x86.R_AX);
      if(emu->x86.R_AL == 1) {
        emu->x86.R_AH = 0x01;
        X86EMU_SET_FLAG(emu, F_CF);
      }
      else {
        emu->x86.R_AH = 0x01;
        X86EMU_SET_FLAG(emu, F_CF);
      }
      break;

    default:
      x86emu_log(emu, "; int 0x13: ah 0x%02x (not implemented)\n", emu->x86.R_AH);

      emu->x86.R_AH = 0x01;
      X86EMU_SET_FLAG(emu, F_CF);
      break;
  }

  return 0;
}


int do_int_15(x86emu_t *emu)
{
  vm_t *vm = emu->private;
  unsigned u, u1, mem_type;
  uint64_t mem_start, mem_size;

  switch(emu->x86.R_AH) {
    case 0x24:
      x86emu_log(emu, "; int 0x15: ah 0x%02x (a20 gate)\n", emu->x86.R_AH);
      switch(emu->x86.R_AL) {
        case 0:
          vm->a20 = 0;
          x86emu_log(emu, "; a20 disabled\n");
          emu->x86.R_AH = 0;
          X86EMU_CLEAR_FLAG(emu, F_CF);
          break;

        case 1:
          vm->a20 = 1;
          x86emu_log(emu, "; a20 enabled\n");
          emu->x86.R_AH = 0;
          X86EMU_CLEAR_FLAG(emu, F_CF);
          break;

        case 2:
          x86emu_log(emu, "; a20 status: %u\n", vm->a20);
          emu->x86.R_AH = 0;
          emu->x86.R_AL = vm->a20;
          X86EMU_CLEAR_FLAG(emu, F_CF);
          break;

        case 3:
          x86emu_log(emu, "; a20 support: 3\n");
          emu->x86.R_AH = 0;
          emu->x86.R_BX = 3;
          X86EMU_CLEAR_FLAG(emu, F_CF);
          break;

        default:
          X86EMU_SET_FLAG(emu, F_CF);
          break;
      }
      break;

    case 0x42:
      x86emu_log(emu, "; int 0x15: ax 0x%04x (thinkpad stuff)\n", emu->x86.R_AX);
      // emu->x86.R_AX = 0x8600;	// ask for F11
      // emu->x86.R_AX = 1;		// start rescue
      break;

    case 0x88:
      x86emu_log(emu, "; int 0x15: ah 0x%02x (ext. mem size)\n", emu->x86.R_AH);
      u = vm->memsize - 1;
      x86emu_log(emu, "; ext mem size: %u MB\n", u);
      if(u > 15) u = 15;
      emu->x86.R_AX = u << 10;
      X86EMU_CLEAR_FLAG(emu, F_CF);
      break;

    case 0xe8:
      if(emu->x86.R_AL == 1) {
        x86emu_log(emu, "; int 0x15: ax 0x%04x (mem map (old))\n", emu->x86.R_AX);
        u = vm->memsize - 1;
        u1 = 0;
        if(u > 15) {
          u = 15;
          u1 = vm->memsize - 16;
        }
        emu->x86.R_AX = emu->x86.R_CX = u << 10;
        emu->x86.R_BX = emu->x86.R_DX = u1 << 4;
        x86emu_log(emu, "; ext mem sizes: %u MB + %u MB\n", u, u1);
        X86EMU_CLEAR_FLAG(emu, F_CF);
      }
      if(emu->x86.R_AL == 0x20 && emu->x86.R_EDX == 0x534d4150) {
        x86emu_log(emu, "; int 0x15: ax 0x%04x (mem map)\n", emu->x86.R_AX);
        emu->x86.R_EAX = 0x534d4150;
        u1 = emu->x86.R_ES_BASE + emu->x86.R_DI;
        mem_type = 0;
        switch(emu->x86.R_EBX) {
          case 0:
            mem_type = 1;
            mem_start = 0;
            mem_size = x86emu_read_word(emu, 0x413) << 10;
            break;

          case 20:
            mem_type = 2;
            mem_start = x86emu_read_word(emu, 0x413) << 10;
            mem_size = (1 << 20) - mem_start;
            break;

          case 40:
            mem_type = 1;
            mem_start = 1 << 20;
            mem_size = (vm->memsize - 1) << 20;
            break;
        }
        if(mem_type) {
          emu->x86.R_EBX += 20;
          if(emu->x86.R_EBX == 60) emu->x86.R_EBX = 0;
          emu->x86.R_ECX = 20;
          vm_write_qword(emu, u1, mem_start);
          vm_write_qword(emu, u1 + 8, mem_size);
          x86emu_write_dword(emu, u1 + 0x10, mem_type);
          x86emu_log(emu, "; mem map: 0x%016llx - 0x%016llx, type %u\n",
            (unsigned long long) mem_start,
            (unsigned long long) (mem_start + mem_size),
            mem_type
          );
          X86EMU_CLEAR_FLAG(emu, F_CF);
        }
        else {
          X86EMU_SET_FLAG(emu, F_CF);
        }
      }
      break;

    default:
      x86emu_log(emu, "; int 0x15: ax 0x%04x\n", emu->x86.R_AX);
      break;
  }

  return 0;
}


int do_int_16(x86emu_t *emu)
{
  vm_t *vm = emu->private;
  int stop = 0;

  switch(emu->x86.R_AH) {
    case 0x00:
    case 0x10:
      x86emu_log(emu, "; int 0x16: ah 0x%02x (get key)\n", emu->x86.R_AH);
      // fprintf(stderr, "opt.keyboard = \"%s\" (block), vm->key = 0x%04x\n", opt.keyboard, vm->key);
      vm->key = vm->key ?: next_bios_key(&opt.keyboard);
      // fprintf(stderr, "vm->key = 0x%04x\n", vm->key);
      emu->x86.R_AX = vm->key;
      if(!vm->key) {
        // we should rather stop here
        x86emu_log(emu, "; blocking key read - stopped\n");
        stop = 1;
      }
      vm->key = 0;
      vm->kbd_cnt = 0;
      break;

    case 0x01:
    case 0x11:
      x86emu_log(emu, "; int 0x16: ah 0x%02x (check for key)\n", emu->x86.R_AH);
      vm->kbd_cnt++;

      if(vm->key || ((vm->kbd_cnt % 4) == 3)) {
        X86EMU_CLEAR_FLAG(emu, F_ZF);
        // fprintf(stderr, "opt.keyboard = \"%s\" (no block)\n", opt.keyboard);
        vm->key = vm->key ?: next_bios_key(&opt.keyboard);
        // fprintf(stderr, "vm->key = 0x%04x\n", vm->key);
        emu->x86.R_AX = vm->key;
        if(!vm->key) X86EMU_SET_FLAG(emu, F_ZF);
      }
      else {
        X86EMU_SET_FLAG(emu, F_ZF);
      }
      break;

    default:
      x86emu_log(emu, "; int 0x16: ah 0x%02x\n", emu->x86.R_AH);
      break;
  }

  return stop;
}


int do_int_19(x86emu_t *emu)
{
//  vm_t *vm = emu->private;

  x86emu_log(emu, "; int 0x19: (boot next device)\n");

  return 1;
}


int do_int_1a(x86emu_t *emu)
{
  // vm_t *vm = emu->private;
  unsigned t;

  switch(emu->x86.R_AH) {
    case 0x00:
      x86emu_log(emu, "; int 0x1a: ah 0x%02x (get system time)\n", emu->x86.R_AH);
      t = x86emu_read_dword(emu, 0x46c);
      x86emu_write_dword(emu, 0x46c, ++t);
      emu->x86.R_DX = t;
      emu->x86.R_CX = t >> 16;
      x86emu_log(emu, "; time = 0x%08x\n", t);
      break;

    default:
      x86emu_log(emu, "; int 0x1a: ah 0x%02x\n", emu->x86.R_AH);
      break;
  }

  return 0;
}


vm_t *vm_new()
{
  vm_t *vm;

  vm = calloc(1, sizeof *vm);

  vm->emu = x86emu_new(X86EMU_PERM_R | X86EMU_PERM_W | X86EMU_PERM_X, 0);
  vm->emu->private = vm;

  if(opt.verbose >= 1 || opt.trace_flags || opt.dump_flags) {
    x86emu_set_log(vm->emu, opt.log_size ?: 100000000, flush_log);
  }

  vm->emu->log.trace = opt.trace_flags;

  x86emu_set_intr_handler(vm->emu, do_int);
  x86emu_set_code_handler(vm->emu, check_ip);

  return vm;
}


void vm_free(vm_t *vm)
{
  x86emu_done(vm->emu);

  free(vm);
}


void vm_run(vm_t *vm)
{
  unsigned flags;

  vm->emu->x86.R_DL = opt.boot;

  if(x86emu_read_word(vm->emu, 0x7c00) == 0) {
    x86emu_log(vm->emu, "# no boot code\n");
    x86emu_clear_log(vm->emu, 1);

    return;
  }

  flags = X86EMU_RUN_LOOP | X86EMU_RUN_NO_CODE;
  if(opt.inst_max) {
    vm->emu->max_instr = opt.inst_max;
    flags |= X86EMU_RUN_MAX_INSTR;
  }

  x86emu_run(vm->emu, flags);

  if(opt.dump_flags) {
    x86emu_log(vm->emu, "\n; - - - emulator state\n");
    x86emu_dump(vm->emu, opt.dump_flags);
    x86emu_log(vm->emu, "; - - -\n");
  }

  x86emu_clear_log(vm->emu, 1);
}


void prepare_bios(vm_t *vm)
{
  unsigned u;
  x86emu_t *emu = vm->emu;

  vm->memsize = 1024;	// 1GB RAM

  // start address 0:0x7c00
  x86emu_set_seg_register(vm->emu, vm->emu->x86.R_CS_SEL, 0);
  vm->emu->x86.R_EIP = 0x7c00;

  x86emu_write_word(emu, 0x413, 640);		// mem size in kB
  x86emu_write_word(emu, 0x417, 0);		// kbd status flags
  x86emu_write_byte(emu, 0x449, 3);		// video mode
  x86emu_write_byte(emu, 0x44a, 80);		// columns
  x86emu_write_byte(emu, 0x484, 24);		// rows - 1
  x86emu_write_byte(emu, 0x485, 16);		// char height
  x86emu_write_byte(emu, 0x462, 0);		// current text page
  x86emu_write_word(emu, 0x450, 0);		// page 0 cursor pos

  x86emu_write_dword(emu, 0x46c, 0);		// time

  vm->bios.iv_base = 0xf8000 + 0x100;
  for(u = 0; u < 0x100; u++) {
    x86emu_write_byte(emu, vm->bios.iv_base + u, 0xcf);	// iret
  }

  x86emu_write_word(emu, 0x10*4, 0x100 + 0x10);
  x86emu_write_word(emu, 0x10*4+2, 0xf800);
  vm->bios.iv_funcs[0x10] = do_int_10;

  x86emu_write_word(emu, 0x11*4, 0x100 + 0x11);
  x86emu_write_word(emu, 0x11*4+2, 0xf800);
  vm->bios.iv_funcs[0x11] = do_int_11;

  x86emu_write_word(emu, 0x12*4, 0x100 + 0x12);
  x86emu_write_word(emu, 0x12*4+2, 0xf800);
  vm->bios.iv_funcs[0x12] = do_int_12;

  x86emu_write_word(emu, 0x13*4, 0x100 + 0x13);
  x86emu_write_word(emu, 0x13*4+2, 0xf800);
  vm->bios.iv_funcs[0x13] = do_int_13;

  x86emu_write_word(emu, 0x15*4, 0x100 + 0x15);
  x86emu_write_word(emu, 0x15*4+2, 0xf800);
  vm->bios.iv_funcs[0x15] = do_int_15;

  x86emu_write_word(emu, 0x16*4, 0x100 + 0x16);
  x86emu_write_word(emu, 0x16*4+2, 0xf800);
  vm->bios.iv_funcs[0x16] = do_int_16;

  x86emu_write_word(emu, 0x19*4, 0x100 + 0x19);
  x86emu_write_word(emu, 0x19*4+2, 0xf800);
  vm->bios.iv_funcs[0x19] = do_int_19;

  x86emu_write_word(emu, 0x1a*4, 0x100 + 0x1a);
  x86emu_write_word(emu, 0x1a*4+2, 0xf800);
  vm->bios.iv_funcs[0x1a] = do_int_1a;
}


int el_torito_boot(x86emu_t *emu, unsigned disk)
{
  unsigned char sector[2048];
  unsigned et, u;
  unsigned start, load_len, load_addr;
  int ok = 0;

  disk_read(emu, 0x7c00, disk, 0x11 * 4, 4, 1);	/* 1 sector from 0x8800 */
  for(u = 0; u < 2048; u++) {
    sector[u] = x86emu_read_byte_noperm(emu, 0x7c00 + u);
  }

  if(
    sector[0] == 0 && sector[6] == 1 &&
    !memcmp(sector + 1, "CD001", 5) &&
    !memcmp(sector + 7, "EL TORITO SPECIFICATION", 23)
  ) {
    et = sector[0x47] + (sector[0x48] << 8) + (sector[0x49] << 16) + (sector[0x4a] << 24);
    lprintf("el_torito_boot: boot catalog at 0x%04x\n", et);
    if(!disk_read(emu, 0x7c00, disk, et * 4, 4, 1)) {
      if(x86emu_read_byte_noperm(emu, 0x7c20) == 0x88) {	/* bootable */
        load_addr = x86emu_read_word(emu, 0x7c22) << 4;
        if(!load_addr) load_addr = 0x7c00;
        load_len = x86emu_read_word(emu, 0x7c26) << 9;
        start = x86emu_read_dword(emu, 0x7c28);

        lprintf(
          "el_torito_boot: load 0x%x bytes from sector 0x%x to 0x%x\n",
          load_len, start, load_addr
        );

        disk_read(emu, load_addr, disk, start * 4, load_len >> 9, 1);
        ok = 1;
      }
    }
  }

  return ok;
}


void prepare_boot(x86emu_t *emu)
{
  if(opt.boot < FIRST_CDROM) {
    disk_read(emu, 0x7c00, opt.boot, 0, 1, 1);
  }
  else {
    el_torito_boot(emu, opt.boot);
  }
}


int disk_read(x86emu_t *emu, unsigned addr, unsigned disk, uint64_t sector, unsigned cnt, int log)
{
  off_t ofs;
  unsigned char *buf;
  unsigned u;

  if(log) x86emu_log(emu, "; read: disk 0x%02x, sector %llu (%u) @ 0x%05x - ",
    disk, (unsigned long long) sector, cnt, addr
  );

  if(disk >= MAX_DISKS || !opt.disk[disk].dev) {
    if(log) x86emu_log(emu, "invalid disk\n");
    return 2;
  }

  if(opt.disk[disk].fd < 0) {
    if(log) x86emu_log(emu, "failed to open disk\n");
    return 3;
  }

  ofs = sector << 9;

  if(lseek(opt.disk[disk].fd, ofs, SEEK_SET) != ofs) {
    if(log) x86emu_log(emu, "sector not found\n");
    return 4;
  }

  buf = malloc(cnt << 9);

  if(read(opt.disk[disk].fd, buf, cnt << 9) != (cnt << 9)) {
    if(log) x86emu_log(emu, "read error\n");
    free(buf);

    return 5;
  }

  for(u = 0; u < cnt << 9; u++) {
    x86emu_write_byte(emu, addr + u, buf[u]);
  }

  free(buf);

  if(log) x86emu_log(emu, "ok\n");

  return 0;
}


int disk_write(x86emu_t *emu, unsigned addr, unsigned disk, uint64_t sector, unsigned cnt, int log)
{
  if(log) x86emu_log(emu, "; write: disk 0x%02x, sector %llu (%u) @ 0x%05x - ",
    disk, (unsigned long long) sector, cnt, addr
  );

  if(disk >= MAX_DISKS || !opt.disk[disk].dev) {
    if(log) x86emu_log(emu, "invalid disk\n");
    return 2;
  }

  // do someting...


  if(log) x86emu_log(emu, "ok\n");

  return 0;
}


void parse_ptable(x86emu_t *emu, unsigned addr, ptable_t *ptable, unsigned base, unsigned ext_base, int entries)
{
  unsigned u;

  memset(ptable, 0, entries * sizeof *ptable);

  for(; entries; entries--, addr += 0x10, ptable++) {
    u = x86emu_read_byte(emu, addr);
    if(u & 0x7f) continue;
    ptable->boot = u >> 7;
    ptable->type = x86emu_read_byte(emu, addr + 4);
    u = x86emu_read_word(emu, addr + 2);
    ptable->start.c = cs2c(u);
    ptable->start.s = cs2s(u);
    ptable->start.h = x86emu_read_byte(emu, addr + 1);
    ptable->start.lin = x86emu_read_dword(emu, addr + 8);
    u = x86emu_read_word(emu, addr + 6);
    ptable->end.c = cs2c(u);
    ptable->end.s = cs2s(u);
    ptable->end.h = x86emu_read_byte(emu, addr + 5);
    ptable->end.lin = ptable->start.lin + x86emu_read_dword(emu, addr + 0xc);

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


void print_ptable_entry(int nr, ptable_t *ptable)
{
  unsigned u;

  if(ptable->valid) {
    lprintf(";     ");
    if(nr > 4 && is_ext_ptable(ptable)) {
      lprintf("  -");
    }
    else {
      lprintf("%3d", nr);
    }

    u = opt.show.rawptable ? 0 : ptable->base;

    lprintf(": %c 0x%02x, start %4u/%3u/%2u %9u, end %4u/%3u/%2u %9u",
      ptable->boot ? '*' : ' ',
      ptable->type,
      ptable->start.c, ptable->start.h, ptable->start.s,
      ptable->start.lin + u,
      ptable->end.c, ptable->end.h, ptable->end.s,
      ptable->end.lin + u
    );

    if(opt.show.rawptable) lprintf(" %+9d", ptable->base);
    lprintf("\n");
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


void dump_ptable(x86emu_t *emu, unsigned disk)
{
  int i, j, pcnt, link_count;
  ptable_t ptable[4], *ptable_ext;
  unsigned s, h, ext_base;

  i = disk_read(emu, 0, disk, 0, 1, 0);

  if(i || x86emu_read_word(emu, 0x1fe) != 0xaa55) {
    lprintf(";     no partition table\n");
    return;
  }

  parse_ptable(emu, 0x1be, ptable, 0, 0, 4);
  i = guess_geo(ptable, 4, &s, &h);
  lprintf(";     partition table (");
  if(i) {
    lprintf("hs %u/%u):\n", h, s);
  }
  else {
    lprintf("inconsistent chs):\n");
  }

  for(i = 0; i < 4; i++) {
    print_ptable_entry(i + 1, ptable + i);
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
      lprintf(";    too many partitions\n");
      break;
    }
    j = disk_read(emu, 0, disk, ptable_ext->start.lin + ptable_ext->base, 1, 0);
    if(j || x86emu_read_word(emu, 0x1fe) != 0xaa55) {
      lprintf(";    ");
      if(j) lprintf("disk read error - ");
      lprintf("not a valid extended partition\n");
      break;
    }
    parse_ptable(emu, 0x1be, ptable, ptable_ext->start.lin + ptable_ext->base, ext_base, 4);
    for(i = 0; i < 4; i++) {
      print_ptable_entry(pcnt, ptable + i);
      if(ptable[i].valid && !is_ext_ptable(ptable + i)) pcnt++;
    }
  }
}


char *get_screen(x86emu_t *emu)
{
  unsigned u, x, y;
  unsigned base = 0xb8000;
  char *s, s_l[80 + 1];
  static char screen[80*25+1];

  *screen = 0;

  for(y = 0; y < 25; y++, base += 80 * 2) {
    for(x = 0; x < 80; x++) {
      u = x86emu_read_byte_noperm(emu, base + 2 * x);
      if(u < 0x20) u = ' ';
      s_l[x] = u;
    }
    s_l[x] = 0;
    for(s = s_l + x - 1; s >= s_l; s--) {
      if(*s != ' ') break;
      *s = 0;
    }

    if(*s_l) strcat(strcat(screen, s_l), "\n");
  }

  return screen;
}


void dump_screen(x86emu_t *emu)
{
  unsigned char *s = (unsigned char *) get_screen(emu);

  lprintf("; - - - screen\n");
  while(*s) {
    lprintf("%s", cp437[*s++]);
  }
  lprintf("; - - -\n");
}


unsigned next_bios_key(char **keys)
{
  char *s, buf[3];
  unsigned char uc;
  unsigned k = 0, u, n;

  // fprintf(stderr, "next_bios_key(\"%s\")\n", *keys);

  if(!keys || !*keys || !**keys) return k;

  if(**keys == '[') {
    (*keys)++;
    s = strchr(*keys, ']');
    if(s) {
      n = s - *keys;
      s++;
      for(u = 0; u < sizeof bios_key_list / sizeof *bios_key_list; u++) {
        if(!strncmp(bios_key_list[u].name, *keys, n)) {
          k = (bios_key_list[u].scan << 8) + bios_key_list[u].ascii;
          break;
        }
      }
    }
    *keys = s;

    // fprintf(stderr, "key = 0x%04x\n", k);

    return k;
  }

  uc = *(*keys)++;

  if(uc == '\\') {
    s = *keys;
    if(*s == 'x' && isxdigit(s[1]) && isxdigit(s[2])) {
      buf[0] = s[1];
      buf[1] = s[2];
      buf[2] = 0;
      *keys += 3;
      uc = strtoul(buf, NULL, 16);
    }
    else {
     uc = *s;
     (*keys)++;
    }
  }

  k = uc;

  for(u = 0; u < sizeof bios_key_list / sizeof *bios_key_list; u++) {
    if(bios_key_list[u].ascii == uc) {
      k = (bios_key_list[u].scan << 8) + uc;
      break;
    }
  }

  // fprintf(stderr, "key = 0x%04x\n", k);

  return k;
}


