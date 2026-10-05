# runtime.mk — geist-runtime (#148), pinned like geistlib and synced by the
# same script. Included by Makefile (geistd links the runtime on the engine)
# and App.mk (geist-app links only the part without the engine).
ifndef GEISTR_CORE
# Rules here must not become the includer's default goal.
runtime_saved_goal := $(.DEFAULT_GOAL)
RUNTIME_REPO ?= https://github.com/geisten/geist-runtime.git
RUNTIME_REF  ?= 3c8f00a9037f9a75c5a9d84ff481f9e321426b80
GEISTR       ?= geist-runtime
ifeq (,$(filter clean distclean,$(MAKECMDGOALS)))
RUNTIME_SYNC := $(shell GEIST_REPO='$(RUNTIME_REPO)' GEIST_REF='$(RUNTIME_REF)' GEISTLIB='$(GEISTR)' \
                        sh scripts/sync-engine.sh >&2 && echo ok)
ifneq ($(RUNTIME_SYNC),ok)
$(error geist-runtime sync failed — see the messages above)
endif
endif
GEISTR_CORE := $(GEISTR)/build/libgeistr-core.a
# The app's compiler when one is given (make -f App.mk APP_CC=…), else the
# engine's: one C23 compiler builds the shared library for both.
GEISTR_CC = $(if $(filter command line,$(origin APP_CC)),$(APP_CC),$(CC))
.PHONY: geistr-core
$(GEISTR_CORE): geistr-core
geistr-core:
	$(MAKE) -C $(GEISTR) core CC="$(GEISTR_CC)"

# The runtime on the engine, compiled against this repository's geistlib.
GEISTR_RUNTIME := $(GEISTR)/build/runtime.o
.PHONY: geistr-runtime
$(GEISTR_RUNTIME): geistr-runtime
geistr-runtime:
	$(MAKE) -C $(GEISTR) build/runtime.o CC="$(CC)" GEISTLIB="$(abspath $(GEISTLIB))" ENGINE_LIB=
.DEFAULT_GOAL := $(runtime_saved_goal)
endif
