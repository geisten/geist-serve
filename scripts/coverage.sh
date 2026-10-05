#!/bin/sh
# coverage.sh [C_MIN] [PYTHON_MIN] [WEB_MIN]: line coverage of this repository
# over `make test`, `make test-app` and the web checks in headless Chrome:
# C (app, geistd, geist-serve and shared code), Python (scripts, workbench,
# desktop shell, client) and the web UI (web/*.js). Every app binary is rebuilt once with clang source-based coverage;
# child processes started by the Python tests inherit LLVM_PROFILE_FILE.
# Continuous mode (%c) keeps the counts of processes the tests kill on purpose.
# Prints the per-file table and fails below MIN_LINE_PERCENT (default: none).
set -eu
cd "$(dirname "$0")/.."
min=${1:-0} py_min=${2:-0} web_min=${3:-0}
tool() { xcrun --find "$1" 2>/dev/null || command -v "$1" || command -v "$1-${LLVM_VERSION:-18}"; }
profdata=$(tool llvm-profdata) || { echo "coverage: llvm-profdata not found" >&2; exit 2; }
cov=$(tool llvm-cov) || { echo "coverage: llvm-cov not found" >&2; exit 2; }
out=build/coverage
rm -rf "${out:?}" && mkdir -p "$out"
# Rebuild this repository's binaries with instrumentation (never the engine library).
rm -rf geist-app geisten geist geist-serve geistd build/geist-app-* build/test_app_* build/test_template build/test_net_signal
flags="-fprofile-instr-generate -fcoverage-mapping"
# Real-inference tests run when the small reference model is present.
if [ -z "${GEIST_TEST_MODEL:-}" ]; then
    for model in "$PWD/geistlib/gguf_artifacts/smollm2-360m-instruct-q8_0.gguf" "$HOME/workspace/geistlib/gguf_artifacts/smollm2-360m-instruct-q8_0.gguf"; do
        [ -f "$model" ] && { GEIST_TEST_MODEL=$model; export GEIST_TEST_MODEL; break; }
    done
fi
export GEIST_EXECUTION_DAEMON="${GEIST_EXECUTION_DAEMON:-$PWD/geistd}"
# Python: every python3 the suites start comes from this venv, whose .pth hook
# starts coverage.py in each process (configuration: .coveragerc).
[ -x build/pyenv/bin/coverage ] || { python3 -m venv build/pyenv && build/pyenv/bin/pip install -q coverage; }
site=$(build/pyenv/bin/python -c 'import sysconfig; print(sysconfig.get_paths()["purelib"])')
echo 'import coverage; coverage.process_startup()' > "$site/geist-coverage.pth"
mkdir -p "$out/py"
export COVERAGE_PROCESS_START="$PWD/.coveragerc" PATH="$PWD/build/pyenv/bin:$PATH"
export LLVM_PROFILE_FILE="$PWD/$out/%p-%m%c.profraw" GEIST_MODEL="${GEIST_MODEL:-${GEIST_TEST_MODEL:-}}"
make COVERAGE_FLAGS="$flags" test
make APP_CC="${APP_CC:-clang}" \
    APP_CFLAGS="-std=c23 -O1 -g -Wall -Wextra -Wpedantic -D_POSIX_C_SOURCE=200809L -D_DARWIN_C_SOURCE $flags" test-app
python3 workbench/test_bench.py
for test in tests/install/*_test.py tests/tools/*_test.py; do python3 "$test"; done
# Build-time generators run before measurement starts; run them once more under it.
python3 scripts/embed-models.py && python3 scripts/embed-tasks.py && python3 scripts/embed-app.py
# The benchmark end to end, one case per task (a quick check; quality refuses it).
if [ -n "${GEIST_TEST_MODEL:-}" ]; then
    rm -rf "${out:?}/bench"
    python3 workbench/bench.py run --candidate smollm2-360m --backend cpu --cases 1 --output "$out/bench" \
        --model-dir "$(dirname "$GEIST_TEST_MODEL")"
    python3 workbench/bench.py report "$out/bench" --json > /dev/null
    ! python3 workbench/bench.py quality "$out/bench" 2>/dev/null
fi
# The web checks drive the instrumented geist-app, so they count for C as well.
web=0
node tests/desktop/chrome_coverage.mjs "$web_min" | tee "$out/web.txt" || web=1
"$profdata" merge -sparse "$out"/*.profraw -o "$out/merged.profdata"
ignore='(tests/|build/|jsmn\.h|clients/|geistlib/|geist-runtime/)'
for binary in geist-app geisten geist-serve geistd build/geist-app-* build/test_app_* build/test_template build/test_net_signal; do
    [ -f "$binary" ] && [ -x "$binary" ] || continue
    "$cov" export "$binary" -instr-profile="$out/merged.profdata" -format=lcov -ignore-filename-regex="$ignore" \
        > "$out/$(basename "$binary").info" 2>/dev/null
done
python3 scripts/coverage_merge.py "$out/lcov.info" "$out"/*.info | sed "s#$PWD/##" | tee "$out/report.txt"
total=$(sed -n 's/^coverage: \([0-9.]*\)% of lines$/\1/p' "$out/report.txt")
build/pyenv/bin/coverage combine --rcfile=.coveragerc -q
# The GTK desktop shell only runs on Linux (tests/desktop/linux_ui_test.py).
omit=''; [ "$(uname -s)" = Linux ] || omit='--omit=desktop/*'
# shellcheck disable=SC2086
build/pyenv/bin/coverage report --rcfile=.coveragerc $omit | tee "$out/python.txt"
py_total=$(awk '/^TOTAL/ {print $NF}' "$out/python.txt" | tr -d %)
status=0
check() { awk -v t="$2" -v m="$3" 'BEGIN { exit !(t + 0 >= m + 0) }' || { echo "coverage: $1 $2% is below $3%" >&2; status=1; }; }
check C "$total" "$min"
check Python "$py_total" "$py_min"
[ $web -eq 0 ] || { echo "coverage: web checks failed or below $web_min%" >&2; status=1; }
echo "coverage: C $total%, Python $py_total%, web $(awk '/TOTAL \(web\)/ {print $1}' "$out/web.txt")"
exit $status
