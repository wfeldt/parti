CC      = gcc
CFLAGS  = -g -O2 -fomit-frame-pointer -Wall
BINDIR  = /usr/bin

GIT2LOG := $(shell if [ -x ./git2log ] ; then echo ./git2log --update ; else echo true ; fi)
GITDEPS := $(shell [ -d .git ] && echo .git/HEAD .git/refs/heads .git/refs/tags)
VERSION := $(shell $(GIT2LOG) --version VERSION ; cat VERSION)
BRANCH  := $(shell git branch | perl -ne 'print $$_ if s/^\*\s*//')
PREFIX  := parti-$(VERSION)

.PHONY: all install package clean

all: changelog parti

changelog: $(GITDEPS)
	$(GIT2LOG) --changelog changelog

parti: parti.c
	$(CC) $(CFLAGS) $< -luuid -o $@

install: parti
	install -m 755 -D parti $(DESTDIR)$(BINDIR)/parti
	ln -snf parti $(DESTDIR)$(BINDIR)/pe

package:
	mkdir -p package
	git archive --prefix=$(PREFIX)/ $(BRANCH) | xz -c > package/$(PREFIX).tar.xz

clean:
	rm -f *~ *.o parti changelog VERSION
	rm -rf package
