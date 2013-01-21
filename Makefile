CC      = gcc
CFLAGS  = -g -O2 -fomit-frame-pointer -Wall
BINDIR  = /usr/bin

GIT2LOG := $(shell if [ -x ./git2log ] ; then echo ./git2log --update ; else echo true ; fi)
GITDEPS := $(shell [ -d .git ] && echo .git/HEAD .git/refs/heads .git/refs/tags)

VERSION := $(shell $(GIT2LOG) --version VERSION ; cat VERSION)
MAJOR_VERSION := $(shell $(GIT2LOG) --version VERSION ; cut -d . -f 1 VERSION)

all: changelog pe

changelog: $(GITDEPS)
	$(GIT2LOG) --changelog changelog

pe: pe.c
	$(CC) $(CFLAGS) $< -o $@

install: pe
	install -m 755 -D pe $(DESTDIR)$(BINDIR)/pe

clean:
	rm -f *~ *.o pe
