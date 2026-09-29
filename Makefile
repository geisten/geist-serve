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
#> make               build ./geist-serve and ./geistd (syncs + builds libgeist.a on demand)
#> make fetch-model   the 369 MB SmolLM2 reference GGUF into the engine tree (SHA-pinned)
#> make test          model-free unit tests + HTTP smoke against a GGUF (skips without one)
#> make format        clang-format, shared style file with the engine
#> make clean         drop the binary; distclean also drops the engine
#>
#> make GEIST_REF=... build against another engine revision, one-off

GEIST_REPO ?= https://github.com/geisten/geistlib.git
GEIST_REF  ?= 33db79d7764b4f6177d944e8ae4fd9f7fadea9be
GEISTLIB   ?= geistlib
MODE       ?= release

NO_ENGINE := clean distclean help format app test-app geist-app build/test_app_core build/test_app_resources
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

ifeq ($(shell uname -s)-$(shell uname -m),Darwin-arm64)
BACKENDS ?= cpu_neon cpu_scalar metal
endif
TARGET ?= $(shell $(GEISTLIB)/mk/detect-target.sh)
include $(GEISTLIB)/mk/target-$(TARGET).mk
include $(GEISTLIB)/mk/gemm-$(GEMM_PROVIDER).mk

endif

empty :=
space := $(empty) $(empty)
ENGINE_FLAVOR := omp$(GEIST_STATIC_OMP)-$(GEMM_PROVIDER)-$(subst $(space),-,$(strip $(BACKENDS)))
ENGINE_SOURCE_ID := $(if $(ENGINE),$(shell python3 scripts/engine-provenance.py identity $(GEISTLIB)),none)
ENGINE_BUILD := build/$(TARGET)/$(MODE)-$(ENGINE_FLAVOR)-$(ENGINE_SOURCE_ID)
ENGINE_LIB := lib/$(TARGET)/$(MODE)-$(ENGINE_FLAVOR)-$(ENGINE_SOURCE_ID)
LIB := $(GEISTLIB)/$(ENGINE_LIB)/libgeist.a

# EXTRA_* are geistlib's own escape hatches; the Linux release binary links
# with EXTRA_LDFLAGS=-static against musl.
CFLAGS  := -std=c23 -O2 -Wall -Wextra -I$(GEISTLIB)/include $(CFLAGS_TARGET) $(GEMM_CFLAGS) $(EXTRA_CFLAGS)
LDFLAGS := $(LDFLAGS_TARGET) $(EXTRA_LDFLAGS)
LDLIBS  := $(LDLIBS_TARGET) $(GEMM_LDLIBS) $(EXTRA_LDLIBS)

.PHONY: all help test fetch-model format clean distclean FORCE

all: geist-serve geistd

help:
	@grep "^#>" Makefile | cut -c4-

SHARED := src/template.c src/json.c src/net.c
HDRS   := src/template.h src/json.h src/net.h src/jsmn.h

geist-serve: src/serve.c $(SHARED) $(HDRS) $(LIB)
	$(CC) $(CFLAGS) -o $@ src/serve.c $(SHARED) $(LIB) $(LDFLAGS) $(LDLIBS)

# geistd: libgeist over a socket for agents (resident sessions, logits).
GEISTD_OUTPUT ?= geistd
DAEMON_SOURCES := $(shell cat scripts/daemon-sources.list)
$(GEISTD_OUTPUT): $(DAEMON_SOURCES) scripts/daemon-sources.list src/lifecycle.h src/resource_sampler.h $(HDRS) $(LIB) scripts/engine-provenance.py
	@mkdir -p $(@D)
	python3 scripts/engine-provenance.py capture $(GEISTLIB) --archive $(LIB) --expected $(ENGINE_SOURCE_ID) --output build/engine-build.h
	$(CC) $(CFLAGS) -Ibuild -o $@ $(DAEMON_SOURCES) $(LIB) $(LDFLAGS) $(LDLIBS)

# Model-free unit test of the chat renderers; no engine needed.
build/test_template: tests/test_template.c src/template.c src/template.h
	@mkdir -p build
	$(CC) -std=c23 -O1 -g -Wall -Wextra -fsanitize=address,undefined -o $@ tests/test_template.c src/template.c

# Always delegate: the engine's own make is incremental, and a plain file
# target goes stale on a GEIST_REF bump.
$(LIB): FORCE
	$(MAKE) -C $(GEISTLIB) lib TARGET=$(TARGET) MODE=$(MODE) \
		GEMM_PROVIDER=$(GEMM_PROVIDER) BACKENDS="$(BACKENDS)" \
		BUILD_DIR="$(ENGINE_BUILD)" LIB_DIR="$(ENGINE_LIB)"

FORCE:

# The CI reference model, fetched and SHA-verified by the engine's own rule
# into geistlib/gguf_artifacts/, which is where tests/smoke.sh looks.
fetch-model:
	$(MAKE) -C $(GEISTLIB) fetch-llama-model

test: geist-serve geistd build/test_template
	./build/test_template
	sh tests/smoke.sh
	sh tests/geistd.sh

format:
	clang-format -i $(wildcard src/*.c tests/*.c)

clean:
	rm -rf geist-serve geistd geist-app build

# The engine checkout is build input, not source.
distclean: clean
	rm -rf $(GEISTLIB)

include App.mk
