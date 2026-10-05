# runtime.mk — geist-runtime (#148), pinned like geistlib and synced by the
# same script. Included by Makefile (geistd links the runtime on the engine)
# and App.mk (geist-app links only the part without the engine).
ifndef GEISTR_CORE
RUNTIME_REPO ?= https://github.com/geisten/geist-runtime.git
RUNTIME_REF  ?= caebbc27bc029d6c1da039cf09ee6f2b26da128d
GEISTR       ?= geist-runtime
ifeq (,$(filter clean distclean,$(MAKECMDGOALS)))
RUNTIME_SYNC := $(shell GEIST_REPO='$(RUNTIME_REPO)' GEIST_REF='$(RUNTIME_REF)' GEISTLIB='$(GEISTR)' \
                        sh scripts/sync-engine.sh >&2 && echo ok)
ifneq ($(RUNTIME_SYNC),ok)
$(error geist-runtime sync failed — see the messages above)
endif
endif
GEISTR_CORE := $(GEISTR)/build/libgeistr-core.a
.PHONY: geistr-core
$(GEISTR_CORE): geistr-core
geistr-core:
	$(MAKE) -C $(GEISTR) core CC="$(APP_CC)"

# The runtime on the engine, compiled against this repository's geistlib.
GEISTR_RUNTIME := $(GEISTR)/build/runtime.o
.PHONY: geistr-runtime
$(GEISTR_RUNTIME): geistr-runtime
geistr-runtime:
	$(MAKE) -C $(GEISTR) build/runtime.o CC="$(CC)" GEISTLIB="$(abspath $(GEISTLIB))" ENGINE_LIB=
endif
