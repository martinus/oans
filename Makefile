# Version from git, falling back to a shipped `version` file (release tarballs
# have no .git) and then "unknown", so the build never fails for lack of it.
ifndef VERSION
VERSION := $(shell git describe --abbrev=4 --dirty --always --tags 2>/dev/null)
ifeq ($(VERSION),)
VERSION := $(shell cat version 2>/dev/null)
endif
ifeq ($(VERSION),)
VERSION := unknown
endif
endif

CC ?= gcc
CFLAGS ?= -Wall -Wextra -Wno-unused-parameter -ggdb -std=gnu11 \
	-Werror=strict-prototypes -MMD
PKG_CONFIG ?= pkg-config

MANPAGE    = docs/man/oans.8
COMPLETION = completion/zsh/_oans

# All C sources live under src/. tests.c is built on its own (the test rule):
# it #includes other .c files to reach their static/inlined code.
CFILES  := $(filter-out src/tests.c,$(sort $(wildcard src/*.c)))
OBJECTS := $(CFILES:.c=.o)
DEPENDS := $(CFILES:.c=.d)

EXTRA_CFLAGS = $(shell $(PKG_CONFIG) --cflags glib-2.0,sqlite3,blkid,mount,uuid,libbsd)
EXTRA_LIBS   = $(shell $(PKG_CONFIG) --libs glib-2.0,sqlite3,blkid,mount,uuid)

ifdef DEBUG
	# We link the system libsqlite3, so SQLITE_* build defines don't apply here.
	DEBUG_FLAGS = -ggdb3 -fsanitize=address -fno-omit-frame-pointer -O0 \
		-DDEBUG_BUILD -fsanitize-address-use-after-scope
else
	# Release hardening (needs optimization, hence not in the debug build).
	# Override with HARDENING= to disable.
	HARDENING ?= -D_FORTIFY_SOURCE=2 -fstack-protector-strong -fstack-clash-protection
	CFLAGS += -O2 $(HARDENING)
	LIBRARY_FLAGS += -Wl,-z,relro -Wl,-z,now
endif

override CFLAGS += -D_FILE_OFFSET_BITS=64 -D_GNU_SOURCE \
	-DVERSTRING=\"$(VERSION)\" $(EXTRA_CFLAGS) $(DEBUG_FLAGS)
LIBRARY_FLAGS += -Wl,--as-needed -latomic -lm $(EXTRA_LIBS)

# make C=1 to check with sparse.
ifdef C
	CC = sparse -D__CHECKER__ -D__CHECK_ENDIAN__ -Wbitwise \
		-Wuninitialized -Wshadow -Wundef
endif

DESTDIR ?= /
PREFIX  ?= /usr/local
BINDIR  = $(PREFIX)/bin
MANDIR  = $(PREFIX)/share/man/man8
ZSHDIR  = $(PREFIX)/share/zsh/site-functions
UNITDIR ?= $(PREFIX)/lib/systemd/system

all: oans

-include $(DEPENDS)

# oans is the only program: src/oans.c has main(), the rest is its library.
oans: $(OBJECTS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $(OBJECTS) -o $@ $(LIBRARY_FLAGS)

# C unit tests: tests.c pulls in the other .c files, so build it standalone.
.PHONY: test
test:
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) src/tests.c -o $@ $(LIBRARY_FLAGS)
	./test

# End-to-end suite (Python stdlib unittest). Dedupe cases need a reflink fs;
# override the scratch dir with DUPEREMOVE_TEST_DIR=/path.
.PHONY: integration
integration: oans
	DUPEREMOVE=./oans python3 tests/run.py

.PHONY: check
check: test integration

# Install oans plus a backward-compatible 'duperemove' symlink, the man page,
# and the zsh completion. `install -D` creates the target directories.
install: oans $(MANPAGE) $(COMPLETION)
	install -D -m 0755 oans $(DESTDIR)$(BINDIR)/oans
	ln -sf oans $(DESTDIR)$(BINDIR)/duperemove
	install -D -m 0644 $(MANPAGE) $(DESTDIR)$(MANDIR)/oans.8
	ln -sf oans.8 $(DESTDIR)$(MANDIR)/duperemove.8
	install -D -m 0644 $(COMPLETION) $(DESTDIR)$(ZSHDIR)/_oans

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/oans $(DESTDIR)$(BINDIR)/duperemove
	rm -f $(DESTDIR)$(MANDIR)/oans.8 $(DESTDIR)$(MANDIR)/duperemove.8
	rm -f $(DESTDIR)$(ZSHDIR)/_oans

# Optional: the systemd@ template units for scheduled dedupe (see
# systemd/README.md). Not part of `install` so it never assumes systemd.
# ExecStart is rewritten to the real install path ($(BINDIR)/oans).
install-systemd:
	install -d $(DESTDIR)$(UNITDIR)
	sed 's|/usr/bin/oans|$(BINDIR)/oans|' systemd/oans@.service \
		> $(DESTDIR)$(UNITDIR)/oans@.service
	install -m 0644 systemd/oans@.timer $(DESTDIR)$(UNITDIR)/oans@.timer

uninstall-systemd:
	rm -f $(DESTDIR)$(UNITDIR)/oans@.service $(DESTDIR)$(UNITDIR)/oans@.timer

# The man page is committed, so building and installing never need pandoc;
# `make doc` regenerates it from the markdown source (for maintainers).
# Prefer a pandoc on PATH, else the one `make pandoc` drops in .pandoc/.
PANDOC = $(or $(shell command -v pandoc 2>/dev/null),$(wildcard $(CURDIR)/.pandoc/pandoc))

.PHONY: doc
doc:
	@test -n "$(PANDOC)" || { echo "No pandoc found. Run 'make pandoc' to fetch a prebuilt one, or install it (see CONTRIBUTING.md)."; exit 1; }
	$(PANDOC) --standalone docs/man/oans.md --to man -o $(MANPAGE)

# Fetch a prebuilt pandoc from PyPI's pypandoc_binary wheel into .pandoc/.
# Works where GitHub release downloads are blocked but PyPI is reachable (e.g.
# CI sandboxes). `make doc` then picks it up automatically; nothing to install.
# Pinned so the fetched pandoc (and thus the .8 it generates) is reproducible
# rather than drifting to whatever is newest; this wheel carries pandoc 3.9.
PYPANDOC_VERSION = 1.17
.PHONY: pandoc
pandoc:
	@mkdir -p .pandoc
	pip download pypandoc_binary==$(PYPANDOC_VERSION) --no-deps -d .pandoc
	@cd .pandoc && unzip -o -q pypandoc_binary-*.whl && cp -f pypandoc/files/pandoc pandoc && chmod +x pandoc
	@echo "Fetched $$(.pandoc/pandoc --version | head -1) -> .pandoc/pandoc"

DIST         = oans-$(VERSION)
DIST_TARBALL = $(VERSION).tar.gz
DIST_SOURCES = $(CFILES) $(sort $(wildcard src/*.h)) LICENSE Makefile \
	README.md docs/man/oans.md docs/nas-quickstart.md $(MANPAGE) $(COMPLETION) \
	systemd/oans@.service systemd/oans@.timer systemd/README.md

# Source tarball with the resolved version embedded, so tarball builds (no
# .git) still report it. --parents keeps the src/, docs/, completion/ layout.
tarball: clean $(DIST_SOURCES)
	tmp=$$(mktemp -d) && mkdir -p "$$tmp/$(DIST)" && \
	cp --parents $(DIST_SOURCES) "$$tmp/$(DIST)" && \
	echo '$(VERSION)' > "$$tmp/$(DIST)/version" && \
	tar -C "$$tmp" -zcf $(DIST_TARBALL) $(DIST) && \
	rm -fr "$$tmp"

clean:
	rm -f $(OBJECTS) $(DEPENDS) oans test test.d $(DIST_TARBALL) *~
