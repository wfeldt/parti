CC      = gcc
CFLAGS  = -g -O2 -fomit-frame-pointer -Wall
# -Wno-pointer-sign -Wsign-conversion -Wsign-compare
LDFLAGS = -ljson-c -luuid -lblkid -lmediacheck
BINDIR  = /usr/bin

GIT2LOG := $(shell if [ -x ./git2log ] ; then echo ./git2log --update ; else echo true ; fi)
GITDEPS := $(shell [ -d .git ] && echo .git/HEAD .git/refs/heads .git/refs/tags)
VERSION := $(shell $(GIT2LOG) --version VERSION ; cat VERSION)
BRANCH  := $(shell git branch | perl -ne 'print $$_ if s/^\*\s*//')
PREFIX  := parti-$(VERSION)

CFLAGS  += -DVERSION=\"$(VERSION)\"

PARTI_SRC = disk.c util.c eltorito.c filesystem.c json.c ptable_apple.c ptable_gpt.c ptable_mbr.c zipl.c
PARTI_OBJ = $(PARTI_SRC:.c=.o)
PARTI_H = $(PARTI_SRC:.c=.h)

.PHONY: all install archive clean

all: changelog parti unify-gpt

changelog: $(GITDEPS)
	$(GIT2LOG) --changelog changelog

$(PARTI_OBJ) parti.o: %.o: %.c $(PARTI_H)
	$(CC) -c $(CFLAGS) $<

parti: parti.o $(PARTI_OBJ)
	$(CC) $^ $(LDFLAGS) -o $@

unify-gpt: unify-gpt.o
	$(CC) $^ -o $@

install: parti
	install -m 755 -D parti $(DESTDIR)$(BINDIR)/parti
	install -m 755 -D unify-gpt $(DESTDIR)$(BINDIR)/unify-gpt

archive: changelog
	mkdir -p package
	git archive --prefix=$(PREFIX)/ $(BRANCH) > package/$(PREFIX).tar
	tar -r -f package/$(PREFIX).tar --mode=0664 --owner=root --group=root --mtime="`git show -s --format=%ci`" --transform='s:^:$(PREFIX)/:' VERSION changelog
	xz -f package/$(PREFIX).tar

clean:
	rm -f *~ *.o parti unify-gpt changelog VERSION
	rm -rf package
