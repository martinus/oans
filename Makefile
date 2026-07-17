# Derive the version from git once (not per-compile), and stay quiet on clones
# without tags: git describe then prints "fatal: No names found" to stderr.
# --always makes VERSION fall back to a short hash; the release check treats a
# missing tag as "not a release".
ifndef VERSION
VERSION := $(shell git describe --abbrev=4 --dirty --always --tags 2>/dev/null)
# Outside a git checkout (e.g. a release tarball) git prints nothing; fall back
# to a shipped `version` file, then a placeholder, so the build never fails and
# still reports a version (#387). `make tarball` writes the file below.
ifeq ($(VERSION),)
VERSION := $(shell cat version 2>/dev/null)
endif
ifeq ($(VERSION),)
VERSION := unknown
endif
endif
ifndef IS_RELEASE
LATEST_TAG := $(shell git describe --abbrev=0 --tags --exclude '*dev' 2>/dev/null)
IS_RELEASE := $(if $(LATEST_TAG),$(if $(filter 0,$(shell git rev-list $(LATEST_TAG)..HEAD --count 2>/dev/null)),1,0),0)
endif

CC ?= gcc
CFLAGS ?= -Wall -ggdb -std=gnu11 -Werror=strict-prototypes -MMD
PKG_CONFIG ?= pkg-config

MANPAGES=docs/man/oans.8 docs/man/btrfs-extent-same.8 docs/man/hashstats.8
ZSH_COMPLETION=completion/zsh/_oans

# All C sources live under src/. tests.c is ugly: it includes lots of c files,
# to get access to inlined code, so it is built separately (see the test rule).
CFILES = $(filter-out src/tests.c,$(sort $(wildcard src/*.c)))
DEPENDS := $(CFILES:.c=.d)
OBJECTS := $(CFILES:.c=.o)
# Main program is 'oans' (compat 'duperemove' symlink added on install).
install_progs = oans hashstats btrfs-extent-same
progs = $(install_progs) csum-test
# The object holding each prog's main() lives at src/<prog>.o.
PROGS_OBJECTS := $(addprefix src/,$(addsuffix .o,$(progs)))
SHARED_OBJECTS := $(filter-out $(PROGS_OBJECTS),$(OBJECTS))

DIST_SOURCES:=$(CFILES) $(sort $(wildcard src/*.h)) LICENSE Makefile \
	README.md $(MANPAGES) docs/oans.html
DIST=oans-$(VERSION)
DIST_TARBALL=$(VERSION).tar.gz
TEMP_INSTALL_DIR:=$(shell mktemp -du -p .)

EXTRA_CFLAGS=$(shell $(PKG_CONFIG) --cflags glib-2.0,sqlite3,blkid,mount,uuid,libbsd)
EXTRA_LIBS=$(shell $(PKG_CONFIG) --libs glib-2.0,sqlite3,blkid,mount,uuid)

ifdef DEBUG
	DEBUG_FLAGS = -ggdb3 -fsanitize=address -fno-omit-frame-pointer	-O0 \
			-DDEBUG_BUILD -DSQLITE_DEBUG -DSQLITE_MEMDEBUG \
			-DSQLITE_ENABLE_EXPLAIN_COMMENTS -fsanitize-address-use-after-scope
else
	CFLAGS += -O2
endif

override CFLAGS += -D_FILE_OFFSET_BITS=64 -DVERSTRING=\"$(VERSION)\" \
	$(EXTRA_CFLAGS) $(DEBUG_FLAGS) \
	-DIS_RELEASE=$(IS_RELEASE) -D_GNU_SOURCE
LIBRARY_FLAGS += -Wl,--as-needed -latomic -lm $(EXTRA_LIBS)

# make C=1 to enable sparse
ifdef C
	CC = sparse -D__CHECKER__ -D__CHECK_ENDIAN__ -Wbitwise \
		-Wuninitialized -Wshadow -Wundef
endif

DESTDIR ?= /
PREFIX ?= /usr/local
SHAREDIR = $(PREFIX)/share
BINDIR = $(PREFIX)/bin
MANDIR = $(SHAREDIR)/man

all: $(progs)
debug:
	@echo "Deprecated, use \"make DEBUG=1\" instead please."

$(MANPAGES): docs/man/%.8: docs/man/%.md
	pandoc --standalone $< --to man -o $@
	pandoc --standalone $< --to html -o docs/$*.html

-include $(DEPENDS)
$(progs): $(OBJECTS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $(SHARED_OBJECTS) src/$@.o -o $@ $(LIBRARY_FLAGS)

.PHONY: test
test:
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) src/tests.c -o $@ $(LIBRARY_FLAGS)
	./test

# End-to-end tests: drive the built binary against a scratch tree and assert on
# the hashfile and on-disk sharing (Python stdlib unittest, no extra deps).
# Needs a reflink-capable scratch fs for the dedupe cases (override with
# DUPEREMOVE_TEST_DIR=/path on e.g. btrfs/xfs).
.PHONY: integration
integration: oans
	DUPEREMOVE=./oans python3 tests/run.py

# Everything: unit tests plus the integration suite.
.PHONY: check
check: test integration

install: $(install_progs) $(MANPAGES) $(ZSH_COMPLETION)
	mkdir -p -m 0755 $(DESTDIR)$(BINDIR)
	for prog in $(install_progs); do \
		install -m 0755 $$prog $(DESTDIR)$(BINDIR); \
	done
	# Backward-compatible 'duperemove' name pointing at 'oans'.
	ln -sf oans $(DESTDIR)$(BINDIR)/duperemove
	mkdir -p -m 0755 $(DESTDIR)$(MANDIR)/man8
	for man in $(MANPAGES); do \
		install -m 0644 $$man $(DESTDIR)$(MANDIR)/man8; \
	done
	ln -sf oans.8 $(DESTDIR)$(MANDIR)/man8/duperemove.8
	mkdir -p -m 0755 $(DESTDIR)$(SHAREDIR)/zsh/site-functions
	for completion in $(ZSH_COMPLETION); do \
		install -m 0644 $$completion $(DESTDIR)$(SHAREDIR)/zsh/site-functions; \
	done

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/duperemove
	for prog in $(install_progs); do \
		rm -f $(DESTDIR)$(BINDIR)/$$prog; \
	done
	rm -f $(DESTDIR)$(MANDIR)/man8/duperemove.8
	for man in $(MANPAGES); do \
		rm -f $(DESTDIR)$(MANDIR)/man8/$${man##*/}; \
	done
	for completion in $(ZSH_COMPLETION); do \
		rm -f $(DESTDIR)$(SHAREDIR)/zsh/site-functions/$${completion##*/}; \
	done

tarball: clean $(DIST_SOURCES)
	mkdir -p $(TEMP_INSTALL_DIR)/$(DIST)
	cp $(DIST_SOURCES) $(TEMP_INSTALL_DIR)/$(DIST)
	# Embed the resolved version so builds from the tarball (no .git) report
	# it instead of falling back to "unknown" (#387).
	echo '$(VERSION)' > $(TEMP_INSTALL_DIR)/$(DIST)/version
	tar -C $(TEMP_INSTALL_DIR) -zcf $(DIST_TARBALL) $(DIST)
	rm -fr $(TEMP_INSTALL_DIR)

clean:
	rm -fr $(OBJECTS) $(progs) $(DIST_TARBALL) $(DEPENDS) *~

doc: $(MANPAGES)
