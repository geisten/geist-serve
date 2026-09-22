# geist-serve — an Ollama- and OpenAI-compatible HTTP front for the geist engine.
#
# The engine is pinned by GEIST_REF below and checked out into $(GEISTLIB) by
# scripts/sync-engine.sh — no git submodule. Every make run verifies the
# checkout against the pin (same scheme as geist-diktat).
#
# Platform knowledge stays in geistlib: its detect-target.sh picks the target
# and its mk/ fragments supply the flags, so geist-serve links with exactly
# what the engine was built with.
#
#> make               build ./geist-serve (syncs + builds libgeist.a on demand)
#> make test          model-free unit tests + --stdio smoke against the CI GGUF
#> make format        clang-format, shared style file with the engine
#> make clean         drop the binary; distclean also drops the engine
#>
#> make GEIST_REF=... build against another engine revision, one-off

GEIST_REPO ?= https://github.com/geisten/geistlib.git
GEIST_REF  ?= 25861c0bd197f1a98f17e49efe0cdc48a0e40713
GEISTLIB   ?= geistlib
MODE       ?= release

NO_ENGINE := clean distclean help format
ifneq (,$(filter-out $(NO_ENGINE),$(or $(MAKECMDGOALS),all)))

ENGINE := $(shell GEIST_REPO='$(GEIST_REPO)' GEIST_REF='$(GEIST_REF)' \
                  GEISTLIB='$(GEISTLIB)' sh scripts/sync-engine.sh >&2 && echo ok)
ifneq ($(ENGINE),ok)
$(error engine sync failed — see the messages above)
endif

# Before the target include: every mk/target-*.mk sets its own
# GEMM_PROVIDER ?=, and ?= only takes while the variable is unset. Every
# artifact we ship is GEMM_PROVIDER=native; `make GEMM_PROVIDER=openblas`
# opts in explicitly.
GEMM_PROVIDER ?= native

TARGET ?= $(shell $(GEISTLIB)/mk/detect-target.sh)
include $(GEISTLIB)/mk/target-$(TARGET).mk
include $(GEISTLIB)/mk/gemm-$(GEMM_PROVIDER).mk

endif

LIB := $(GEISTLIB)/lib/$(TARGET)/$(MODE)/libgeist.a

# EXTRA_* are geistlib's own escape hatches; the Linux release binary links
# with EXTRA_LDFLAGS=-static against musl.
CFLAGS  := -std=c23 -O2 -Wall -Wextra -I$(GEISTLIB)/include $(CFLAGS_TARGET) $(GEMM_CFLAGS) $(EXTRA_CFLAGS)
LDFLAGS := $(LDFLAGS_TARGET) $(EXTRA_LDFLAGS)
LDLIBS  := $(LDLIBS_TARGET) $(GEMM_LDLIBS) $(EXTRA_LDLIBS)

.PHONY: all help test format clean distclean FORCE

all: geist-serve

help:
	@grep "^#>" Makefile | cut -c4-

geist-serve: src/serve.c $(LIB)
	$(CC) $(CFLAGS) -o $@ $< $(LIB) $(LDFLAGS) $(LDLIBS)

# Always delegate: the engine's own make is incremental, and a plain file
# target goes stale on a GEIST_REF bump.
$(LIB): FORCE
	$(MAKE) -C $(GEISTLIB) lib TARGET=$(TARGET) MODE=$(MODE) \
		GEMM_PROVIDER=$(GEMM_PROVIDER)

FORCE:

test: geist-serve
	sh tests/smoke.sh

format:
	clang-format -i $(wildcard src/*.c tests/*.c)

clean:
	rm -f geist-serve

# The engine checkout is build input, not source.
distclean: clean
	rm -rf $(GEISTLIB)
