#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <getopt.h>
#include <inttypes.h>

void help(void);

struct option options[] = {
  { "help",       0, NULL, 'h'  },
  { "verbose",    0, NULL, 'v'  },
  { "disk",       1, NULL, 1001 },
  { }
};

struct {
  unsigned verbose;
} opt;


int main(int argc, char **argv)
{
  int i;

  opterr = 0;

  while((i = getopt_long(argc, argv, "hv", options, NULL)) != -1) {
    switch(i) {
      case 'v':
        opt.verbose++;
        break;

      default:
        help();
        return i == 'h' ? 0 : 1;
    }

  }

  return 0;
}


void help()
{
  fprintf(stderr,
    "Partition Edit\nUsage: pe OPTIONS\n"
    "\n"
    "Options:\n"
    "  --verbose\n"
    "      more logs\n"
    "  --help\n"
    "      show this text\n"
  );
}


