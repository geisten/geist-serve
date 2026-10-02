# The application builds independently of geistlib. libcurl supplies TLS;
# macOS uses system CommonCrypto, Linux uses OpenSSL's maintained SHA-256.
APP_CC ?= cc
APP_CFLAGS ?= -std=c23 -O2 -Wall -Wextra -Wpedantic -D_POSIX_C_SOURCE=200809L -D_DARWIN_C_SOURCE
APP_LDLIBS ?= -lcurl -lpthread
ifeq ($(shell uname -s),Linux)
APP_LDLIBS += -lcrypto -lm
endif
# geist-app itself: app.h holds the shared state these files split between them.
APP_MAIN := src/app/main.c src/app/request.c src/app/child.c src/app/prefs.c src/app/observe.c src/app/jobs.c src/app/status.c src/app/chat.c src/app/compare.c src/app/routes.c
APP_SOURCE := src/app/memory.c src/app/engine.c src/app/catalog.c src/app/core.c src/app/platform.c src/app/resources.c
APP_RUNTIME := src/lifecycle.c src/app/activity.c src/app/output.c src/app/performance.c src/app/daemon.c src/template.c src/app/tasks.c src/app/compat.c src/app/connection.c
APP_HEADERS := src/app/app.h src/app/memory.h src/lifecycle.h src/app/activity.h src/app/engine.h src/app/output.h src/app/performance.h build/app_models.h src/app/resources.h src/app/version.h src/app/tasks.h build/app_tasks.h src/app/daemon.h src/app/compat.h src/app/connection.h clients/geistd_client.h src/jsmn.h src/template.h src/json.h
.PHONY: app test-app
app: geist-app geist
geist: build/app_models.h src/app/catalog.c src/app/version.h src/app/cli.c src/app/connection.c src/app/connection.h src/app/core.c src/app/core.h src/json.c src/json.h
	$(APP_CC) $(APP_CFLAGS) -Isrc -o $@ src/app/cli.c src/app/connection.c src/app/core.c src/app/catalog.c src/json.c $(APP_LDLIBS)
geist-app: $(APP_MAIN) $(APP_SOURCE) $(APP_RUNTIME) $(APP_HEADERS) src/app/core.h src/json.c src/json.h web/index.html web/app.css web/app.js web/i18n.js web/markdown.js web/vendor/marked.umd.js web/vendor/katex.min.js build/app_assets.h
	$(APP_CC) $(APP_CFLAGS) -Isrc -o $@ $(APP_MAIN) $(APP_SOURCE) $(APP_RUNTIME) src/json.c $(APP_LDLIBS)
build/test_app_core: build/app_models.h tests/app/core_test.c $(APP_SOURCE) src/app/core.h
	@mkdir -p build
	$(APP_CC) $(APP_CFLAGS) -g -O1 -fsanitize=address,undefined -o $@ tests/app/core_test.c $(APP_SOURCE) src/json.c $(APP_LDLIBS)
test-app: build/test_app_memory_journal build/test_app_memory build/test_app_lifecycle build/test_app_activity build/test_app_engine build/test_app_output build/test_app_engine_identity build/test_app_performance build/test_app_resources build/geist-app-old build/geist-app-new build/geist-app-legacy build/geist-app-test geist-app geist build/test_app_core build/test_app_client build/test_app_tasks
	@# The CLI and lifecycle tests start geist-app, which needs a real geistd (#70).
	@test -x "$${GEIST_EXECUTION_DAEMON:-geistd}" || { echo "test-app needs ./geistd (or GEIST_EXECUTION_DAEMON): run 'make' first."; exit 1; }
	./build/test_app_memory
	python3 -c 'import tempfile,subprocess; d=tempfile.TemporaryDirectory(); [subprocess.run(["./build/test_app_memory_journal",d.name,mode],check=True) for mode in ("write","read")]'
	./build/test_app_lifecycle
	./build/test_app_activity
	python3 tests/app/activity_test.py
	python3 tests/app/lifecycle_fault_test.py
	./build/test_app_engine
	python3 tests/app/engine_provenance_test.py
	./build/test_app_output
	./build/test_app_resources
	./build/test_app_core
	./build/test_app_tasks
	python3 tests/app/client_test.py
	python3 tests/app/reasoning_test.py
	python3 tests/app/eval_test.py
	python3 tests/app/quality_test.py
	python3 tests/app/deadline_test.py
	python3 tests/app/http_test.py
	python3 tests/app/catalog_test.py
	python3 tests/app/verification_test.py
	python3 tests/app/engine_identity_test.py
	python3 tests/app/performance_test.py
	python3 tests/app/performance_real_test.py
	python3 tests/app/background_download_test.py
	python3 tests/app/variants_test.py
	python3 tests/app/execution_test.py
	python3 tests/app/bonsai_test.py
	python3 tests/app/remove_test.py
	python3 tests/app/setup_test.py
	python3 tests/app/setup_cli_test.py
	python3 tests/app/state_test.py
	python3 tests/app/i18n_server_test.py
	python3 tests/app/compat_test.py
	python3 tests/app/chat_test.py
	python3 tests/app/cli_test.py
	GEIST_OLD_APP="$(CURDIR)/build/geist-app-old" GEIST_NEW_APP="$(CURDIR)/build/geist-app-new" GEIST_LEGACY_APP="$(CURDIR)/build/geist-app-legacy" python3 tests/app/upgrade_test.py

build/geist-app-test: $(APP_MAIN) $(APP_SOURCE) $(APP_RUNTIME) $(APP_HEADERS) src/app/core.h src/json.c web/index.html web/app.css web/app.js web/i18n.js web/markdown.js web/vendor/marked.umd.js web/vendor/katex.min.js build/app_assets.h
	@mkdir -p build
	$(APP_CC) $(APP_CFLAGS) -DAPP_TESTING -g -fsanitize=address,undefined -Isrc -o $@ $(APP_MAIN) $(APP_SOURCE) $(APP_RUNTIME) src/json.c $(APP_LDLIBS)

build/app_assets.h: web/index.html web/app.css web/app.js web/i18n.js web/markdown.js web/vendor/marked.umd.js web/vendor/katex.min.js scripts/embed-app.py
	python3 scripts/embed-app.py

build/test_app_client: tests/app/client_probe.c clients/geistd_client.h src/jsmn.h
	@mkdir -p build
	$(APP_CC) $(APP_CFLAGS) -Isrc -g -fsanitize=address,undefined -o $@ $<

build/app_tasks.h: models/catalog.json tasks/catalog.json scripts/embed-tasks.py $(wildcard quality/*.json quality/*.py quality/bundles/*/*.json quality/bundles/*/*.jsonl) $(wildcard src/*.c src/*.h src/app/*.c src/app/*.h)
	python3 scripts/embed-tasks.py

build/test_app_tasks: tests/app/tasks_test.c src/app/tasks.c src/app/tasks.h build/app_tasks.h
	$(APP_CC) $(APP_CFLAGS) -g -fsanitize=address,undefined -o $@ tests/app/tasks_test.c src/app/tasks.c

# The test fixtures differ only in the version reported by the real service.
build/geist-app-old build/geist-app-new build/geist-app-legacy: $(APP_MAIN) $(APP_SOURCE) $(APP_RUNTIME) $(APP_HEADERS) build/app_assets.h
	$(APP_CC) $(APP_CFLAGS) -DAPP_VERSION='"$(if $(filter build/geist-app-old,$@),0.4.9,$(if $(filter build/geist-app-new,$@),0.6.0,))"' -Isrc -o $@ $(APP_MAIN) $(APP_SOURCE) $(APP_RUNTIME) src/json.c $(APP_LDLIBS)

# OS resource counters and sampling invariants, including the Linux parser on Mac.
build/test_app_resources: tests/app/resources_test.c src/app/resources.c src/app/resources.h
	@mkdir -p build
	$(APP_CC) $(APP_CFLAGS) -g -O1 -fsanitize=address,undefined -o $@ tests/app/resources_test.c src/app/resources.c

build/app_models.h: models/catalog.json scripts/embed-models.py
	python3 scripts/embed-models.py

build/test_app_performance: tests/app/performance_test.c src/app/performance.c src/app/performance.h $(APP_SOURCE) src/app/core.h src/json.c
	$(APP_CC) $(APP_CFLAGS) -DAPP_TESTING -g -O1 -fsanitize=address,undefined -o $@ tests/app/performance_test.c src/app/performance.c $(APP_SOURCE) src/json.c $(APP_LDLIBS)

build/test_app_engine_identity: tests/app/engine_identity_test.c $(APP_SOURCE) src/app/core.h build/app_models.h src/json.c
	$(APP_CC) $(APP_CFLAGS) -g -O1 -fsanitize=address,undefined -o $@ tests/app/engine_identity_test.c $(APP_SOURCE) src/json.c $(APP_LDLIBS)

build/test_app_output: tests/app/output_test.c src/app/output.c src/app/output.h
	$(APP_CC) $(APP_CFLAGS) -g -fsanitize=address,undefined -o $@ tests/app/output_test.c src/app/output.c

build/test_app_engine: tests/app/engine_test.c $(APP_SOURCE) src/json.c
	@mkdir -p build
	$(APP_CC) $(APP_CFLAGS) -g -fsanitize=address,undefined -o $@ $^ $(APP_LDLIBS)

build/test_app_activity: tests/app/activity_test.c src/app/activity.c src/app/activity.h $(APP_SOURCE)
	$(APP_CC) $(APP_CFLAGS) -g -fsanitize=address,undefined -Isrc -o $@ tests/app/activity_test.c src/app/activity.c $(APP_SOURCE) src/json.c $(APP_LDLIBS)

build/test_app_lifecycle: tests/app/lifecycle_test.c src/lifecycle.c src/lifecycle.h
	$(APP_CC) $(APP_CFLAGS) -g -O1 -fsanitize=address,undefined -o $@ tests/app/lifecycle_test.c src/lifecycle.c $(APP_LDLIBS)

build/test_app_memory: tests/app/memory_test.c $(APP_SOURCE) src/json.c build/app_models.h
	$(APP_CC) $(APP_CFLAGS) -g -O1 -fsanitize=address,undefined -o $@ tests/app/memory_test.c $(APP_SOURCE) src/json.c $(APP_LDLIBS)

build/test_app_memory_journal: tests/app/memory_journal_test.c src/app/performance.c src/app/performance.h $(APP_SOURCE) src/json.c build/app_models.h
	$(APP_CC) $(APP_CFLAGS) -g -O1 -fsanitize=address,undefined -o $@ tests/app/memory_journal_test.c src/app/performance.c $(APP_SOURCE) src/json.c $(APP_LDLIBS)
