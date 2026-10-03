#!/bin/sh
# install-geist.sh — install the shared Geisten runtime (geist, geist-app, geistd)
# for the current Linux user, without root. Contract: docs/INSTALL-LINUX.md.
#
#   curl -fsSL https://geisten.net/install.sh | sh
#   sh install-geist.sh --help
#
# Everything runs from main() on the last line, so a truncated download only
# defines functions and changes nothing.

GEIST_RELEASES=https://github.com/geisten/geist-serve/releases
# Ed25519 key that signs geist-manifest (docs/INSTALL-LINUX.md, "Signing").
# Empty would make installation refuse; rotation: same section.
GEIST_MANIFEST_PUBKEY='-----BEGIN PUBLIC KEY-----
MCowBQYDK2VwAyEATOqns8uqh+mBxILUi21Mq67Rcx3Mq3t0ZvLKKtwR+jg=
-----END PUBLIC KEY-----'

E_USAGE=2 E_HOST=10 E_PREREQ=11 E_VERIFY=12 E_OWNER=13 E_BUSY=14 E_SETUP=15 E_LOCAL=16 E_MODEL=17

usage() {
    cat <<'EOF'
Install the Geisten model runtime for this user (no root needed).

  sh install-geist.sh [options]

  --version X.Y.Z   install exactly this release (default: newest stable)
  --no-start        install, but do not start the Geisten service
  --model recommended
                    then set up the model Geisten recommends for this computer
                    (asks first; with --yes it downloads without asking)
  --dry-run         check, download and verify only; change nothing
  --uninstall       remove the installed runtime (models and settings stay)
  --yes             no prompts (never implies root, deletion or overwriting)
  --help            this text

Exit codes: 0 ok, 2 usage, 10 unsupported host, 11 missing prerequisite,
12 download or verification failed, 13 another installation owns the
files, 14 Geisten is busy, 15 start failed, 16 local file system problem,
17 installed, but the model setup did not finish (run: geist setup).
EOF
}

say() { printf '%s\n' "$*"; }
fail() {
    code=$1
    shift
    printf 'geist install: %s\n' "$*" >&2
    cleanup
    exit "$code"
}
cleanup() {
    [ -n "${stage:-}" ] && [ -d "$stage" ] && rm -rf "$stage"
    [ -n "${lock:-}" ] && [ -d "$lock" ] && [ "$(cat "$lock/pid" 2>/dev/null)" = "$$" ] && rm -rf "$lock"
    return 0
}

parse_args() {
    want_version='' no_start=0 dry_run=0 uninstall=0 model='' yes=0
    while [ $# -gt 0 ]; do
        case $1 in
            --help | -h) usage; exit 0 ;;
            --version)
                [ $# -ge 2 ] || fail $E_USAGE "--version needs X.Y.Z"
                want_version=$2
                shift
                ;;
            --no-start) no_start=1 ;;
            --dry-run) dry_run=1 ;;
            --uninstall) uninstall=1 ;;
            --yes) yes=1 ;;
            --model)
                [ "${2:-}" = recommended ] || fail $E_USAGE "--model takes: recommended"
                model=recommended
                shift
                ;;
            --desktop) fail $E_USAGE "$1 is not available yet (see geisten/geist-serve#46)" ;;
            *) fail $E_USAGE "unknown option: $1 (see --help)" ;;
        esac
        shift
    done
    if [ -n "$want_version" ]; then
        case $want_version in
            *[!0-9.]* | .* | *. | *..*) fail $E_USAGE "--version needs X.Y.Z" ;;
        esac
        [ "$(printf '%s' "$want_version" | tr -cd . | wc -c)" -eq 2 ] || fail $E_USAGE "--version needs X.Y.Z"
    fi
    [ $uninstall -eq 1 ] && { [ -n "$want_version" ] || [ $dry_run -eq 1 ] || [ -n "$model" ]; } &&
        fail $E_USAGE "--uninstall takes no other options"
    [ -n "$model" ] && [ $no_start -eq 1 ] && fail $E_USAGE "--model needs the service: drop --no-start"
    return 0
}

# Local fixtures only through this explicit switch; production is HTTPS only.
test_mode() { [ -n "${GEIST_INSTALL_TEST_ORIGIN:-}" ]; }

preflight() {
    [ "$(id -u)" != 0 ] || fail $E_HOST "run this as the user who will use Geisten, not as root"
    case $(uname -s) in
        Linux) ;;
        Darwin) fail $E_HOST "on macOS, install the Geisten app from its DMG instead" ;;
        *) fail $E_HOST "unsupported system $(uname -s): Geisten needs 64-bit Linux" ;;
    esac
    machine=$(uname -m)
    test_mode && [ -n "${GEIST_INSTALL_TEST_ARCH:-}" ] && machine=$GEIST_INSTALL_TEST_ARCH
    case $machine in
        x86_64 | amd64) platform=linux-x86_64 elf_machine='3e 00' ;;
        aarch64 | arm64) platform=linux-aarch64 elf_machine='b7 00' ;;
        *) fail $E_HOST "unsupported CPU architecture $machine (x86_64 or aarch64 needed)" ;;
    esac
    [ "$(getconf LONG_BIT 2>/dev/null)" = 64 ] || fail $E_HOST "a 64-bit userland is needed (this one is 32-bit)"
    cpuinfo=/proc/cpuinfo
    test_mode && [ -n "${GEIST_INSTALL_TEST_CPUINFO:-}" ] && cpuinfo=$GEIST_INSTALL_TEST_CPUINFO
    if [ $platform = linux-x86_64 ]; then
        need='avx avx2 bmi1 bmi2 f16c fma abm movbe' what='x86-64-v3 (AVX2, FMA, BMI2)'
        flags=$(grep -m1 '^flags' "$cpuinfo" 2>/dev/null)
    else
        need='asimddp fphp asimdhp' what='ARMv8.2 with dot product and FP16'
        flags=$(grep -m1 '^Features' "$cpuinfo" 2>/dev/null)
    fi
    for f in $need; do
        case " ${flags#*:} " in *" $f "*) ;; *) fail $E_HOST "this CPU lacks $f: the engine needs $what" ;; esac
    done
    missing=''
    for tool in curl openssl tar gzip sha256sum mktemp od base64 grep; do
        command -v "$tool" >/dev/null 2>&1 || missing="$missing $tool"
    done
    [ -z "$missing" ] || fail $E_PREREQ "missing:$missing (Debian/Ubuntu: sudo apt install curl openssl tar gzip coreutils)"
    case $(openssl version 2>/dev/null) in
        "OpenSSL 3"* | "OpenSSL 4"*) ;;
        *) fail $E_PREREQ "OpenSSL 3 or newer is needed to verify the download" ;;
    esac
    mv -T --help >/dev/null 2>&1 || fail $E_PREREQ "GNU coreutils mv (with -T) is needed"
    [ -n "${HOME:-}" ] && [ -d "$HOME" ] && [ -w "$HOME" ] || fail $E_LOCAL "HOME must be a writable directory"
    runtime=${XDG_DATA_HOME:-$HOME/.local/share}/geist-runtime
    bindir=$HOME/.local/bin
    launcher=$bindir/geist
}

# One owner per machine user: never shadow the Ubuntu package, never touch a
# foreign file at our launcher path.
check_ownership() {
    if [ -e /usr/lib/geist/geist ] || [ -L /usr/bin/geist ]; then
        fail $E_OWNER "Geisten is installed from the Ubuntu package; update it with apt, or remove it first (sudo apt remove geist)"
    fi
    if [ -e "$launcher" ] || [ -L "$launcher" ]; then
        [ -L "$launcher" ] && [ "$(readlink "$launcher")" = "$runtime/current/geist" ] ||
            fail $E_OWNER "$launcher exists and was not installed by this installer; move it away first"
    fi
    if [ -e "$runtime" ] && [ ! -f "$runtime/receipt" ]; then
        for entry in "$runtime"/* "$runtime"/.[!.]*; do
            [ -e "$entry" ] || [ -L "$entry" ] || continue
            [ "${entry##*/}" = .lock ] && continue
            fail $E_OWNER "$runtime exists without an installer receipt; move it away first"
        done
    fi
}

take_lock() {
    mkdir -p "$runtime" || fail $E_LOCAL "cannot create $runtime"
    chmod 700 "$runtime"
    lock=$runtime/.lock
    if ! mkdir "$lock" 2>/dev/null; then
        holder=$(cat "$lock/pid" 2>/dev/null)
        case $holder in '' | *[!0-9]*) holder='' ;; esac
        if [ -n "$holder" ] && kill -0 "$holder" 2>/dev/null; then
            lock=''
            fail $E_BUSY "another Geisten installation is running (pid $holder)"
        fi
        rm -rf "$lock"
        mkdir "$lock" || { lock=''; fail $E_LOCAL "cannot take the installer lock"; }
    fi
    echo $$ >"$lock/pid"
}

fetch() { # url out max-bytes
    if test_mode; then
        proto='=https,http,file'
    else
        proto='=https'
    fi
    curl --proto "$proto" --proto-redir '=https' -fsSL --connect-timeout 20 --max-time 600 \
        --retry 3 --max-filesize "$3" -o "$2" "$1" || fail $E_VERIFY "download failed: $1"
}

# Ed25519 signature over the exact manifest bytes, key pinned above.
verify_manifest() {
    pubkey=$GEIST_MANIFEST_PUBKEY
    test_mode && [ -n "${GEIST_INSTALL_TEST_PUBKEY:-}" ] && pubkey=$(cat "$GEIST_INSTALL_TEST_PUBKEY")
    [ -n "$pubkey" ] || fail $E_VERIFY "no release signing key is configured in this installer yet"
    printf '%s\n' "$pubkey" >"$stage/key.pem"
    base64 -d "$stage/manifest.sig" >"$stage/sig.bin" 2>/dev/null || fail $E_VERIFY "malformed manifest signature"
    [ "$(wc -c <"$stage/sig.bin")" -eq 64 ] || fail $E_VERIFY "malformed manifest signature"
    openssl pkeyutl -verify -pubin -inkey "$stage/key.pem" -rawin -in "$stage/manifest" \
        -sigfile "$stage/sig.bin" >/dev/null 2>&1 || fail $E_VERIFY "manifest signature is not valid"
}

# Strict line format; see docs/INSTALL-LINUX.md. No eval, no source.
parse_manifest() {
    m=$stage/manifest
    grep -q "$(printf '\r')" "$m" && fail $E_VERIFY "manifest has CR line endings"
    [ -z "$(tail -c1 "$m")" ] || fail $E_VERIFY "manifest must end with a newline"
    set -f
    n=0 version='' channel='' source='' engine='' archive='' size='' digest='' count=0
    while IFS= read -r line; do
        n=$((n + 1))
        # shellcheck disable=SC2086 # split the line into fields; globbing is off (set -f)
        set -- $line
        case $n in
            1) [ "$line" = "geist-manifest 1" ] || fail $E_VERIFY "unknown manifest format" ;;
            2) [ "$line" = "product geist" ] || fail $E_VERIFY "manifest is not for Geisten" ;;
            3) [ $# -eq 2 ] && [ "$1" = version ] || fail $E_VERIFY "bad manifest version line"
               version=$2 ;;
            4) [ $# -eq 2 ] && [ "$1" = channel ] || fail $E_VERIFY "bad manifest channel line"
               channel=$2 ;;
            5) [ $# -eq 2 ] && [ "$1" = source ] || fail $E_VERIFY "bad manifest source line"
               source=$2 ;;
            6) [ $# -eq 2 ] && [ "$1" = engine ] || fail $E_VERIFY "bad manifest engine line"
               engine=$2 ;;
            *) [ $# -eq 6 ] && [ "$1" = archive ] || fail $E_VERIFY "unexpected manifest line $n"
               if [ "$2" = "$platform" ]; then
                   count=$((count + 1)) archive=$4 size=$5 digest=$6
               fi ;;
        esac
    done <"$m"
    set +f
    [ $n -ge 7 ] || fail $E_VERIFY "manifest lists no archives"
    case $version in *[!0-9.]* | '') fail $E_VERIFY "bad version in manifest" ;; esac
    case $source in *[!0-9a-f]* | '') fail $E_VERIFY "bad source revision in manifest" ;; esac
    [ ${#source} -eq 40 ] || fail $E_VERIFY "bad source revision in manifest"
    [ "$channel" = stable ] || fail $E_VERIFY "manifest channel is $channel, not stable"
    [ -z "$want_version" ] || [ "$version" = "$want_version" ] || fail $E_VERIFY "manifest is for $version, not $want_version"
    [ $count -eq 1 ] || fail $E_VERIFY "manifest has $count archives for $platform"
    [ "$archive" = "geist-$version-$platform.tar.gz" ] || fail $E_VERIFY "unexpected archive name $archive"
    case $size in *[!0-9]* | '') fail $E_VERIFY "bad archive size" ;; esac
    case $digest in *[!0-9a-f]* | '') fail $E_VERIFY "bad archive digest" ;; esac
    [ ${#digest} -eq 64 ] || fail $E_VERIFY "bad archive digest"
    [ "$size" -le 268435456 ] || fail $E_VERIFY "archive larger than 256 MiB"
}

# Only these regular files, in one top directory; nothing else may extract.
allowed_member() {
    case $1 in
        "$top/" | "$top/geist" | "$top/geist-app" | "$top/geistd" | "$top/Start Geisten.sh" | \
            "$top/LICENSE" | "$top/marked-LICENSE" | "$top/katex-LICENSE" | "$top/ENGINE.json" | \
            "$top/README.md" | "$top/SHA256SUMS" | "$top/BUILD-PACKAGES.txt") return 0 ;;
    esac
    return 1
}

verify_archive() {
    a=$stage/archive.tar.gz
    [ "$(wc -c <"$a")" -eq "$size" ] || fail $E_VERIFY "archive size differs from the manifest"
    [ "$(sha256sum "$a" | cut -d' ' -f1)" = "$digest" ] || fail $E_VERIFY "archive checksum differs from the manifest"
    top=geist-$version-$platform
    tar -tzf "$a" >"$stage/names" 2>/dev/null || fail $E_VERIFY "archive is not a readable tar.gz"
    [ "$(wc -l <"$stage/names")" -le 16 ] || fail $E_VERIFY "archive has too many members"
    [ "$(sort "$stage/names" | uniq -d | wc -l)" -eq 0 ] || fail $E_VERIFY "archive has duplicate members"
    while IFS= read -r name; do
        allowed_member "$name" || fail $E_VERIFY "unexpected archive member: $name"
    done <"$stage/names"
    tar -tvzf "$a" >"$stage/listing" 2>/dev/null || fail $E_VERIFY "archive is not a readable tar.gz"
    # First column is the type: only regular files (-) and directories (d).
    [ -z "$(cut -c1 "$stage/listing" | tr -d -- '-d\n')" ] || fail $E_VERIFY "archive contains links or special files"
    total=$(awk '{ s += $3 } END { printf "%d", s }' "$stage/listing")
    [ "$total" -le 536870912 ] || fail $E_VERIFY "archive expands beyond 512 MiB"
    mkdir "$stage/x" && chmod 700 "$stage/x"
    tar -xzf "$a" -C "$stage/x" --no-same-owner --no-same-permissions 2>/dev/null ||
        fail $E_VERIFY "archive could not be extracted"
    payload=$stage/x/$top
    for bin in geist geist-app geistd; do
        [ -f "$payload/$bin" ] && [ ! -L "$payload/$bin" ] || fail $E_VERIFY "archive lacks $bin"
        [ "$(od -An -tx1 -j18 -N2 "$payload/$bin" | tr -s ' ' | sed 's/^ //')" = "$elf_machine" ] ||
            fail $E_VERIFY "$bin is not built for $platform"
        chmod 755 "$payload/$bin"
    done
    (cd "$payload" && sha256sum -c --quiet SHA256SUMS >/dev/null 2>&1) || fail $E_VERIFY "payload checksums do not match"
    "$payload/geist" --help >/dev/null 2>&1
    rc=$?
    [ $rc -eq 132 ] && fail $E_HOST "this CPU cannot run the Geisten engine (illegal instruction)"
    [ $rc -eq 0 ] || fail $E_VERIFY "the downloaded geist does not run here (exit $rc)"
}

write_receipt() {
    cat >"$runtime/receipt.new" <<EOF
receipt 1
version $version
platform $platform
source $source
engine $engine
launcher $launcher
previous ${previous:-none}
EOF
    mv -Tf "$runtime/receipt.new" "$runtime/receipt" || fail $E_LOCAL "cannot write the receipt"
}

receipt_value() { # key
    [ -f "$runtime/receipt" ] || return 1
    while IFS=' ' read -r key value; do
        [ "$key" = "$1" ] && { printf '%s' "$value"; return 0; }
    done <"$runtime/receipt"
    return 1
}

point_current_at() { # version
    if ! { ln -s "versions/$1" "$runtime/current.new" && mv -Tf "$runtime/current.new" "$runtime/current"; }; then
        fail $E_LOCAL "cannot switch the active version"
    fi
}

activate() {
    previous=$(receipt_value version) || previous=''
    mkdir -p "$runtime/versions" "$bindir" || fail $E_LOCAL "cannot create $runtime/versions or $bindir"
    dest=$runtime/versions/$version
    if [ -d "$dest" ]; then
        if ! (cd "$dest" && sha256sum -c --quiet SHA256SUMS >/dev/null 2>&1); then
            mv -T "$dest" "$runtime/versions/.broken-$version-$$" || fail $E_LOCAL "cannot replace $dest"
            mv -T "$payload" "$dest" || fail $E_LOCAL "cannot install into $dest"
        fi
    else
        mv -T "$payload" "$dest" || fail $E_LOCAL "cannot install into $dest"
    fi
    point_current_at "$version"
    if [ ! -L "$launcher" ]; then
        if ! { ln -s "$runtime/current/geist" "$launcher.new" && mv -Tf "$launcher.new" "$launcher"; }; then
            fail $E_LOCAL "cannot create $launcher"
        fi
    fi
    # A same-version rerun keeps the recorded rollback version.
    if [ "$previous" = "$version" ]; then
        previous=$(receipt_value previous) || previous=''
        [ "$previous" = none ] && previous=''
    fi
    write_receipt
}

# Keep the active and the previous version; remove only installer-owned others.
prune() {
    for d in "$runtime"/versions/* "$runtime"/versions/.broken-*; do
        [ -d "$d" ] || continue
        v=${d##*/}
        [ "$v" = "$version" ] || [ "$v" = "${previous:-}" ] || rm -rf "$d"
    done
}

start_service() {
    "$launcher" start >"$stage/start.log" 2>&1
    rc=$?
    if [ $rc -eq 0 ] && "$launcher" connection >/dev/null 2>&1; then
        return 0
    fi
    if [ -n "${previous:-}" ] && [ -d "$runtime/versions/$previous" ]; then
        point_current_at "$previous"
        version=$previous previous=''
        write_receipt
    fi
    [ $rc -eq 43 ] && fail $E_BUSY "Geisten is busy; your models are untouched. Run this installer again when it is idle"
    cat "$stage/start.log" >&2
    fail $E_SETUP "the new version did not start; the previous installation is active again"
}

# The first model (#46). `geist setup` asks on /dev/tty and follows the
# service's own recommendation; this script never picks a model itself.
setup_model() {
    if [ -n "$model" ]; then
        if [ $yes -eq 1 ]; then "$launcher" setup --yes; else "$launcher" setup; fi ||
            fail $E_MODEL "Geisten is installed, but the model setup did not finish. Resume with: $cmd setup"
    # A subshell: a failed redirection on a special builtin would end this shell (POSIX).
    elif [ $fresh -eq 1 ] && (true </dev/tty) 2>/dev/null; then
        "$launcher" setup || say "Geisten is installed; the model setup did not finish. Resume with: $cmd setup"
    else
        say "Choose and load a model:   $cmd setup"
    fi
}

do_uninstall() {
    [ -f "$runtime/receipt" ] || fail $E_OWNER "no installer receipt in $runtime: nothing to uninstall"
    if "$runtime/current/geist" status >"$runtime/.status" 2>/dev/null; then
        if grep -q '"busy":true\|"loading":true' "$runtime/.status"; then
            rm -f "$runtime/.status"
            fail $E_BUSY "Geisten is busy; run --uninstall again when it is idle"
        fi
        "$runtime/current/geist" stop >/dev/null 2>&1 || fail $E_BUSY "Geisten could not be stopped"
    fi
    rm -f "$runtime/.status"
    [ -L "$launcher" ] && [ "$(readlink "$launcher")" = "$runtime/current/geist" ] && rm -f "$launcher"
    rm -rf "$runtime"
    say "Geisten runtime removed. Models and settings stay in ${XDG_DATA_HOME:-$HOME/.local/share}/geist."
}

main() {
    parse_args "$@"
    stage='' lock=''
    trap 'cleanup; exit 130' INT
    trap 'cleanup; exit 143' TERM
    test_mode && say "TEST MODE: fetching from $GEIST_INSTALL_TEST_ORIGIN"
    preflight
    if [ $uninstall -eq 1 ]; then
        take_lock
        do_uninstall
        exit 0
    fi
    check_ownership
    fresh=0
    [ -f "$runtime/receipt" ] || fresh=1
    take_lock
    stage=$(mktemp -d "$runtime/.stage.XXXXXX") || fail $E_LOCAL "cannot create a staging directory"
    chmod 700 "$stage"
    if test_mode; then
        base=$GEIST_INSTALL_TEST_ORIGIN
    elif [ -n "$want_version" ]; then
        base=$GEIST_RELEASES/download/v$want_version
    else
        base=$GEIST_RELEASES/latest/download
    fi
    say "Checking the release…"
    fetch "$base/geist-manifest" "$stage/manifest" 8192
    fetch "$base/geist-manifest.sig" "$stage/manifest.sig" 512
    verify_manifest
    parse_manifest
    say "Downloading Geisten $version for $platform…"
    test_mode || base=$GEIST_RELEASES/download/v$version
    fetch "$base/$archive" "$stage/archive.tar.gz" "$size"
    say "Verifying…"
    verify_archive
    if [ $dry_run -eq 1 ]; then
        say "Dry run: Geisten $version verified; would install into $runtime/versions/$version and link $launcher."
        cleanup
        exit 0
    fi
    say "Installing…"
    activate
    [ $no_start -eq 1 ] || start_service
    prune
    cleanup
    say "Geisten $version is installed."
    case ":$PATH:" in
        *":$bindir:"*) cmd=geist ;;
        *) cmd="'$launcher'"
           say "Note: $bindir is not on your PATH. Use the full path below, or add it to PATH in your shell profile." ;;
    esac
    [ $no_start -eq 1 ] && say "Start it with:   $cmd start"
    say "Open the model manager:   $cmd open"
    [ $no_start -eq 1 ] || setup_model
    exit 0
}

main "$@"
