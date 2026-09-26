# The application builds independently of geistlib. libcurl supplies TLS;
# macOS uses system CommonCrypto, Linux uses OpenSSL's maintained SHA-256.
APP_CC ?= cc
APP_CFLAGS ?= -std=c23 -O2 -Wall -Wextra -Wpedantic -D_POSIX_C_SOURCE=200809L -D_DARWIN_C_SOURCE
APP_LDLIBS ?= -lcurl -lpthread
ifeq ($(shell uname -s),Linux)
APP_LDLIBS += -lcrypto
endif
APP_SOURCE := src/app/core.c src/app/platform.c
APP_RUNTIME := src/app/daemon.c src/template.c src/app/tasks.c src/app/compat.c src/app/connection.c
APP_HEADERS := src/app/tasks.h build/app_tasks.h src/app/daemon.h src/app/compat.h src/app/connection.h clients/geistd_client.h src/jsmn.h src/template.h src/json.h
.PHONY: app test-app
app: geist-app geist
geist: src/app/cli.c src/app/connection.c src/app/connection.h src/app/core.c src/app/core.h src/json.c src/json.h
	$(APP_CC) $(APP_CFLAGS) -Isrc -o $@ src/app/cli.c src/app/connection.c src/app/core.c src/json.c $(APP_LDLIBS)
geist-app: src/app/main.c $(APP_SOURCE) $(APP_RUNTIME) $(APP_HEADERS) src/app/core.h src/json.c src/json.h web/index.html web/app.css web/app.js build/app_assets.h
	$(APP_CC) $(APP_CFLAGS) -Isrc -o $@ src/app/main.c $(APP_SOURCE) $(APP_RUNTIME) src/json.c $(APP_LDLIBS)
build/test_app_core: tests/app/core_test.c $(APP_SOURCE) src/app/core.h
	@mkdir -p build
	$(APP_CC) $(APP_CFLAGS) -g -O1 -fsanitize=address,undefined -o $@ tests/app/core_test.c $(APP_SOURCE) $(APP_LDLIBS)
test-app: geist-app geist build/test_app_core build/test_app_client build/test_app_tasks
	./build/test_app_core
	./build/test_app_tasks
	python3 tests/app/client_test.py
	python3 tests/app/eval_test.py
	python3 tests/app/quality_test.py
	python3 tests/app/deadline_test.py
	python3 tests/app/http_test.py
	python3 tests/app/compat_test.py
	python3 tests/app/cli_test.py

build/geist-app-test: src/app/main.c $(APP_SOURCE) $(APP_RUNTIME) $(APP_HEADERS) src/app/core.h src/json.c web/index.html web/app.css web/app.js build/app_assets.h
	@mkdir -p build
	$(APP_CC) $(APP_CFLAGS) -DAPP_TESTING -g -fsanitize=address,undefined -Isrc -o $@ src/app/main.c $(APP_SOURCE) $(APP_RUNTIME) src/json.c $(APP_LDLIBS)

build/app_assets.h: web/index.html web/app.css web/app.js scripts/embed-app.py
	python3 scripts/embed-app.py

build/test_app_client: tests/app/client_probe.c clients/geistd_client.h src/jsmn.h
	@mkdir -p build
	$(APP_CC) $(APP_CFLAGS) -Isrc -g -fsanitize=address,undefined -o $@ $<

build/app_tasks.h: tasks/catalog.json scripts/embed-tasks.py $(wildcard quality/*.json quality/*.py quality/bundles/*/*.json quality/bundles/*/*.jsonl) $(wildcard src/*.c src/*.h src/app/*.c src/app/*.h)
	python3 scripts/embed-tasks.py

build/test_app_tasks: tests/app/tasks_test.c src/app/tasks.c src/app/tasks.h build/app_tasks.h
	$(APP_CC) $(APP_CFLAGS) -g -fsanitize=address,undefined -o $@ tests/app/tasks_test.c src/app/tasks.c
