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

# Extra warnings the tree is already clean under. Not every compiler knows every
# flag (-Wduplicated-cond, -Wduplicated-branches, -Wlogical-op and
# -Wjump-misses-init are GCC-only), and clang turns an unknown -W into an error
# once -Werror is on, so probe each against $(CC) and keep what it accepts.
# Assigned with := so the probes run once, not on every CFLAGS expansion.
cc-option = $(shell $(CC) -Werror $(1) -E -x c /dev/null >/dev/null 2>&1 && echo $(1))
WARN_EXTRA := $(foreach w,-Wundef -Wvla -Wstrict-overflow=2 \
	-Wduplicated-cond -Wduplicated-branches -Wlogical-op -Wjump-misses-init \
	-Wold-style-definition -Wmissing-include-dirs,$(call cc-option,$(w)))

# src/fiemap.c embeds a variable-sized `struct fiemap` ahead of its extent array
# - the layout the ioctl wants, and a deliberate GNU extension under -std=gnu11.
# clang diagnoses it, gcc does not. Probe the *positive* flag (gcc rejects it) so
# only clang is handed the -Wno- form instead of gcc carrying a dead option.
WARN_EXTRA += $(if $(call cc-option,-Wgnu-variable-sized-type-not-at-end),\
	-Wno-gnu-variable-sized-type-not-at-end)

# make WERROR=1 to make any warning fatal. CI builds this way so a warning can
# never land silently; locally `scripts/verify.sh` greps the build log instead.
ifdef WERROR
	override CFLAGS += -Werror
endif

MANPAGE    = docs/man/oans.8
COMPLETION = completion/zsh/_oans

# All C sources live under src/, and all of them belong to the binary: the unit
# suite lives under tests/unit/ instead, so this glob needs no exception.
CFILES  := $(sort $(wildcard src/*.c))
OBJECTS := $(CFILES:.c=.o)
DEPENDS := $(CFILES:.c=.d)

# The unit suite, whose sources are under tests/unit/. TEST_INLINED are the ones
# a test #includes to reach a `static` function; they must not *also* be linked,
# or every symbol in them is defined twice. Everything else the suite needs is
# an ordinary object it shares with the binary.
TEST_INLINED := file_scan.c progress.c find_dupes.c dbfile.c hash-tree.c \
		results-tree.c fiemap.c glob.c csum.c interrupt.c
TEST_SKIP    := $(addprefix src/,$(TEST_INLINED:.c=.o)) src/oans.o src/run_dedupe.o
TEST_LINKED  := $(filter-out $(TEST_SKIP),$(OBJECTS))
TEST_SOURCES := $(sort $(wildcard tests/unit/tu_*.c)) tests/unit/main.c
TEST_OBJECTS := $(TEST_SOURCES:.c=.o)
TEST_DEPENDS := $(TEST_SOURCES:.c=.d)

EXTRA_CFLAGS = $(shell $(PKG_CONFIG) --cflags glib-2.0,sqlite3,blkid,mount,uuid,libbsd)
EXTRA_LIBS   = $(shell $(PKG_CONFIG) --libs glib-2.0,sqlite3,blkid,mount,uuid)

ifdef DEBUG
	# We link the system libsqlite3, so SQLITE_* build defines don't apply here.
	DEBUG_FLAGS = -ggdb3 -fsanitize=address -fno-omit-frame-pointer -O0 \
		-DDEBUG_BUILD -fsanitize-address-use-after-scope
else ifdef SANITIZE
	# Sanitizer build, e.g. `make SANITIZE=address,undefined CC=clang`. The
	# flags land in CFLAGS, which the link rules also use, so -fsanitize
	# instruments and links in one shot. -fno-sanitize-recover=all makes UBSAN
	# *abort* on a finding (it only logs by default) so the test suites catch
	# it; ASAN aborts already. -O1 keeps stacks readable while staying fast
	# enough for the integration suite. No release hardening here: FORTIFY's
	# interceptors clash with ASAN's and only warn under these options.
	DEBUG_FLAGS = -ggdb3 -O1 -fno-omit-frame-pointer \
		-fsanitize=$(SANITIZE) -fno-sanitize-recover=all \
		-DSANITIZER_BUILD
	# Env for running the instrumented `./test` and `./oans`: abort on any
	# finding (so a suite fails on it), print UBSAN stacks, and point
	# LeakSanitizer at the suppressions for GLib's cached idle-thread residue
	# (the same library false positive tests/valgrind.supp filters). Prepended
	# by the `test`/`integration` rules; empty (a no-op) in a non-sanitized build.
	SANITIZE_RUN = \
		ASAN_OPTIONS=abort_on_error=1:detect_leaks=1:detect_stack_use_after_return=1:strict_string_checks=1 \
		UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
		LSAN_OPTIONS=suppressions=$(CURDIR)/tests/lsan.supp

	# ThreadSanitizer needs two extra things (src/tsan.h explains why):
	#   - the GLib annotations, -include'd rather than #include'd so they sit
	#     ahead of every translation unit without touching any call site.
	#   - LOCK_MEMSTATS: memstats.h's counters are deliberately unlocked
	#     outside DEBUG_BUILD, a real if benign race that would bury the
	#     reports worth reading.
	ifneq (,$(findstring thread,$(SANITIZE)))
		DEBUG_FLAGS += -include $(CURDIR)/src/tsan.h -DLOCK_MEMSTATS
		SANITIZE_RUN += \
			TSAN_OPTIONS=suppressions=$(CURDIR)/tests/tsan.supp:halt_on_error=0:second_deadlock_stack=1
	endif
else
	# Release hardening (needs optimization, hence not in the debug build).
	# Override with HARDENING= to disable.
	HARDENING ?= -D_FORTIFY_SOURCE=2 -fstack-protector-strong -fstack-clash-protection
	CFLAGS += -O2 $(HARDENING)
	LIBRARY_FLAGS += -Wl,-z,relro -Wl,-z,now
endif

override CFLAGS += $(WARN_EXTRA) -D_FILE_OFFSET_BITS=64 -D_GNU_SOURCE \
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

-include $(DEPENDS) $(TEST_DEPENDS)

# Rebuild the version-stamped object when the version *string* changes, not only
# when its source does. A release is just a tag plus a man-page bump, so `make`
# would otherwise leave the previous VERSTRING baked into src/oans.o and
# `oans --version` would report the old version on an otherwise-current build.
# The stamp records $(VERSION) and is rewritten only when it actually changes
# (the cmp guard), so it never churns the build otherwise. VERSTRING is used
# only in src/oans.c; if another source uses it, add its object here too.
.PHONY: force-version
.version-stamp: force-version
	@printf '%s\n' '$(VERSION)' | cmp -s - $@ 2>/dev/null || printf '%s\n' '$(VERSION)' > $@

src/oans.o: .version-stamp

# oans is the only program: src/oans.c has main(), the rest is its library.
oans: $(OBJECTS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $(OBJECTS) -o $@ $(LIBRARY_FLAGS)

# C unit tests: one translation unit per subject, so that changing one source
# rebuilds one of them rather than all of them. That is what a mutation sweep
# pays for - measured, a mutant went from 1.44 s of compiling to about 0.35 s,
# because scripts/mutate/mutate.py reuses a lane across mutants and make can
# then do an incremental build inside it.
#
# TEST_INLINED are the sources a test #includes to reach a `static` function.
# They must not *also* be linked: every symbol in them would be defined twice.
# Everything else the suite needs is an ordinary object, shared with the binary.
tests/unit/%.o: tests/unit/%.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc -Itests/unit -c $< -o $@

#
# Building and running are separate targets because a harness that reads the
# exit status has to tell the two apart. scripts/mutate/mutate.py builds a
# mutated tree and then runs the suite; with one command doing both, a mutant
# the tests caught would exit nonzero at the *build* step and be reported as
# one the compiler refused - crediting the compiler with protection the tests
# provided, which is the direction that tool must never be wrong in.
# `make test` is unchanged for everyone else.
.PHONY: test-build
test-build: $(TEST_OBJECTS) $(TEST_LINKED)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $(TEST_OBJECTS) $(TEST_LINKED) \
		-o test $(LIBRARY_FLAGS)

.PHONY: test
test: test-build
	$(SANITIZE_RUN) ./test

# End-to-end suite (Python stdlib unittest). Dedupe cases need a reflink fs;
# override the scratch dir with DUPEREMOVE_TEST_DIR=/path.
#
# Worker processes for the suite - tests/run.py's -j, see `tests/run.py --help`.
# Not make's own -j; TEST_JOBS=1 is the sequential fallback. The same value
# sizes the valgrind leg below, where each worker is far heavier.
TEST_JOBS ?= auto
.PHONY: integration
integration: oans
	$(SANITIZE_RUN) DUPEREMOVE=./oans python3 tests/run.py -j $(TEST_JOBS)

# Same end-to-end suite, but every oans invocation runs under valgrind memcheck
# (via tests/valgrind-wrap.sh). Findings go to per-pid logs; a non-empty log
# means a real error/leak, so we fail if any survived. ~7x slower than plain
# `integration` - opt-in, not part of `check`. Needs valgrind installed.
VGLOGDIR = $(CURDIR)/.vglogs
.PHONY: integration-valgrind
# TEST_SHARD=I/N runs only that slice, so CI can spread this leg (~90% of the
# job's wall time) over parallel runners. Unset runs everything.
integration-valgrind: oans
	@command -v valgrind >/dev/null 2>&1 || { echo "valgrind not installed"; exit 1; }
	rm -rf $(VGLOGDIR) && mkdir -p $(VGLOGDIR)
	OANS_VG_LOGDIR=$(VGLOGDIR) DUPEREMOVE=tests/valgrind-wrap.sh python3 tests/run.py \
		-j $(TEST_JOBS) $(if $(TEST_SHARD),--shard $(TEST_SHARD))
	@if find $(VGLOGDIR) -type f -size +0c | grep -q .; then \
		echo "=== valgrind reported errors/leaks ==="; \
		find $(VGLOGDIR) -type f -size +0c -exec cat {} +; \
		exit 1; \
	fi; \
	echo "valgrind: no findings"

# Source invariants that a compiler cannot express, each with its own waiver
# syntax (see the scripts):
#   longpath (#117) - no syscall may take a scanned file's path as a single
#     argument, or it silently ENAMETOOLONGs and the file is dropped;
#   escape (#202) - no scanned file's name may be printed unescaped, or a
#     crafted name rewrites the terminal.
#
# Plus one invariant about the tree rather than the sources: mutate_core.py is
# vendored from unordered_dense, where its test suite lives, so a local edit
# here would leave this repository running something nothing tests.
.PHONY: lint
lint:
	@python3 scripts/lint-longpath.py
	@python3 scripts/lint-escape.py
	@python3 scripts/lint-test-registry.py
	@python3 scripts/lint-mutate-core.py
	@python3 scripts/mutate/test_report.py

# Replay the known bugs in scripts/mutate/bugs/ and fail if any survives - the
# check that says the tests would still notice #147, #159, #186, #187, #191 and
# #202. Each bug file names the source it mutates on its own first line, so this
# loop has nothing to know. Not in `check`: it is minutes rather than seconds,
# and it is what the CI `mutation` job runs.
.PHONY: mutation-replay
mutation-replay:
	@status=0; for bugs in scripts/mutate/bugs/*.txt; do \
		echo "=== $$bugs ==="; \
		python3 scripts/mutate/mutate.py --bugs "$$bugs" \
			$(MUTATE_ARGS) || status=1; \
	done; exit $$status

.PHONY: check
check: lint test integration

# Everything CI runs, in one command. Each leg rebuilds from scratch on purpose:
# the sanitizer builds use incompatible flags, and a stale object from a previous
# leg fails to link (undefined __ubsan_handle_* / __tsan_* symbols). Ordered
# fastest-failing first, so a plain compile error costs seconds rather than the
# whole run (~7 min with -j on 16 cores; TSAN and valgrind are most of it).
#
# Sanitizer legs must go through make, not a bare `python3 tests/run.py`: the
# SANITIZE_RUN above is what exports the ASAN/UBSAN/LSAN/TSAN options, and
# without them TSAN runs with no suppressions and reports GLib's internals as
# races. Use SAN_CC= to override the sanitizer compiler (clang by default; gcc
# has no ThreadSanitizer support for this configuration).
SAN_CC ?= clang

.PHONY: check-all
check-all:
	@command -v $(SAN_CC) >/dev/null 2>&1 || \
		{ echo "check-all needs $(SAN_CC) for the sanitizer legs (override with SAN_CC=)"; exit 1; }
	@command -v valgrind >/dev/null 2>&1 || { echo "check-all needs valgrind"; exit 1; }
	@echo "=== [1/5] build + lint + unit + integration ==="
	$(MAKE) clean && $(MAKE) check
	@echo "=== [2/5] AddressSanitizer ==="
	$(MAKE) clean && $(MAKE) integration SANITIZE=address CC=$(SAN_CC)
	@echo "=== [3/5] UndefinedBehaviorSanitizer ==="
	$(MAKE) clean && $(MAKE) integration SANITIZE=undefined CC=$(SAN_CC)
	@echo "=== [4/5] ThreadSanitizer ==="
	$(MAKE) clean && $(MAKE) integration SANITIZE=thread CC=$(SAN_CC)
	@echo "=== [5/5] valgrind memcheck ==="
	$(MAKE) clean && $(MAKE) integration-valgrind
	@echo
	@echo "check-all: all 5 legs passed"

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
	rm -f $(OBJECTS) $(DEPENDS) $(TEST_OBJECTS) $(TEST_DEPENDS) oans test test.d $(DIST_TARBALL) .version-stamp *~
