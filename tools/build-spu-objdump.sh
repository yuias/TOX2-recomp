#!/usr/bin/env bash
# Builds the SPU-capable objdump (GNU binutils, --target=spu-elf) inside WSL.
#
# GNU binutils is only executed as a separate process by the SPU decoder cross-check; nothing of it is copied
# into the repository.
#
# Usage, from the repository root (the script is fed through stdin, so WSL needs no access to the repository):
#   wsl.exe -d Ubuntu-26.04 -e bash -s -- [--from TARBALL] [--force] < tools/build-spu-objdump.sh
# Git Bash rewrites arguments that look like POSIX paths, so a command that passes a WSL path to --from needs
# the prefix MSYS_NO_PATHCONV=1.
#
# The distro is chosen by the caller (-d NAME): Ubuntu-26.04 is the one verified to have gcc and make. The
# script runs inside the distro and cannot select one itself.
#
#   --from TARBALL  WSL path of a local copy of the pinned tarball; it gets the same SHA-256 check and is never
#                   modified or deleted. Checked even when the install is already up to date.
#   --force         Discard the install and build again.
#
# Exit codes: 0 success (including no-op), 1 failure, 2 usage error.

BINUTILS_VERSION="2.47"
BINUTILS_URL="https://ftp.gnu.org/gnu/binutils/binutils-2.47.tar.xz"
BINUTILS_SHA256="154ab23b60070e8f27013c22977f1129425d67d1e8acd6e13010e617811e4cff"
# Binutils only: no gas, ld, gold, gprof, gprofng, gdb or sim. Werror is off because newer compilers warn on
# this old code; the output is only ever run, not shipped.
CONFIGURE_FLAGS=(--target=spu-elf --disable-nls --disable-werror --disable-gas --disable-ld --disable-gold
    --disable-gprof --disable-gprofng --disable-gdb --disable-gdbserver --disable-sim --disable-libdecnumber
    --disable-readline)
# Stops make from needing texinfo; the documentation is not built.
MAKE_ARGS=(MAKEINFO=true)

CACHE_DIR="$HOME/.cache/tox2-spu-binutils"
PREFIX="$HOME/.local/opt/tox2-spu-binutils"
OBJDUMP="$PREFIX/bin/spu-elf-objdump"
MARKER="$PREFIX/.tox2-pin"

die() {
    echo "spu-objdump: $*" >&2
    exit 1
}

usage_error() {
    echo "spu-objdump: $*" >&2
    echo "usage: wsl.exe -d Ubuntu-26.04 -e bash -s -- [--from TARBALL] [--force] < tools/build-spu-objdump.sh" >&2
    exit 2
}

main() {
    set -eu
    set -o pipefail

    local from="" force=0
    while [ $# -gt 0 ]; do
        case "$1" in
            --from)
                [ $# -ge 2 ] && [ -n "$2" ] || usage_error "--from needs a path"
                from="$2"
                shift 2
                ;;
            --force)
                force=1
                shift
                ;;
            *)
                usage_error "unexpected argument: $1"
                ;;
        esac
    done

    [ -n "${HOME:-}" ] && [ "$HOME" != "/" ] || die "HOME is not set to a usable directory"

    # Prerequisites first, so a missing tool does not leave a half-done download behind.
    local tool pkg missing_pkgs=""
    for tool in gcc g++ make curl xz tar sha256sum; do
        command -v "$tool" >/dev/null 2>&1 && continue
        case "$tool" in
            gcc | g++ | make) pkg="build-essential" ;;
            xz) pkg="xz-utils" ;;
            sha256sum) pkg="coreutils" ;;
            *) pkg="$tool" ;;
        esac
        case " $missing_pkgs " in
            *" $pkg "*) ;;
            *) missing_pkgs="$missing_pkgs $pkg" ;;
        esac
    done
    if [ -n "$missing_pkgs" ]; then
        echo "spu-objdump: missing build tools; run this in the distro, then run the script again:" >&2
        echo "  sudo apt install${missing_pkgs}" >&2
        exit 1
    fi

    local tarball="" tarball_sum
    if [ -n "$from" ]; then
        [ -f "$from" ] || die "--from file not found: $from"
        tarball_sum="$(sha256sum "$from")"
        tarball_sum="${tarball_sum%% *}"
        if [ "$tarball_sum" != "$BINUTILS_SHA256" ]; then
            die "SHA-256 mismatch for $from: expected $BINUTILS_SHA256, got $tarball_sum"
        fi
        tarball="$from"
    fi

    local pin="$BINUTILS_VERSION $BINUTILS_SHA256"
    if [ "$force" -eq 0 ] && [ -x "$OBJDUMP" ] && [ -f "$MARKER" ] && [ "$(cat "$MARKER")" = "$pin" ]; then
        echo "spu-objdump: up to date (binutils $BINUTILS_VERSION)"
        return 0
    fi

    mkdir -p "$CACHE_DIR/dl"
    # Global, because the EXIT trap runs after main has returned.
    WORK_DIR=""
    trap 'if [ -n "$WORK_DIR" ]; then rm -rf "$WORK_DIR"; fi' EXIT

    if [ -z "$tarball" ]; then
        tarball="$CACHE_DIR/dl/binutils-$BINUTILS_VERSION.tar.xz"
        local have=0
        if [ -f "$tarball" ]; then
            tarball_sum="$(sha256sum "$tarball")"
            tarball_sum="${tarball_sum%% *}"
            if [ "$tarball_sum" = "$BINUTILS_SHA256" ]; then
                have=1
                echo "spu-objdump: using the downloaded tarball $tarball"
            else
                echo "spu-objdump: discarding $tarball (SHA-256 does not match the pin)"
                rm -f "$tarball"
            fi
        fi
        if [ "$have" -eq 0 ]; then
            echo "spu-objdump: downloading $BINUTILS_URL"
            curl --fail --silent --show-error --location --proto '=https' --tlsv1.2 --retry 3 \
                --output "$tarball.part" "$BINUTILS_URL" </dev/null
            tarball_sum="$(sha256sum "$tarball.part")"
            tarball_sum="${tarball_sum%% *}"
            if [ "$tarball_sum" != "$BINUTILS_SHA256" ]; then
                rm -f "$tarball.part"
                die "SHA-256 mismatch for the download: expected $BINUTILS_SHA256, got $tarball_sum"
            fi
            mv -f "$tarball.part" "$tarball"
        fi
    fi

    # A failed build must not leave a marker that makes the next run a no-op.
    rm -f "$MARKER"
    if [ "$force" -eq 1 ]; then
        rm -rf "$PREFIX"
    fi

    local work
    work="$(mktemp -d "$CACHE_DIR/work.XXXXXX")"
    WORK_DIR="$work"
    echo "spu-objdump: extracting"
    tar --no-same-owner -xJf "$tarball" -C "$work" </dev/null
    local src="$work/binutils-$BINUTILS_VERSION"
    [ -f "$src/configure" ] || die "the tarball does not contain binutils-$BINUTILS_VERSION/configure"

    mkdir "$work/build"
    echo "spu-objdump: configuring"
    (cd "$work/build" && "$src/configure" --prefix="$PREFIX" "${CONFIGURE_FLAGS[@]}" </dev/null >configure.log 2>&1) ||
        { tail -n 30 "$work/build/configure.log" >&2; die "configure failed (log: $work/build/configure.log)"; }

    echo "spu-objdump: building"
    (cd "$work/build" && make -j"$(nproc)" all-binutils "${MAKE_ARGS[@]}" </dev/null >make.log 2>&1) ||
        { tail -n 30 "$work/build/make.log" >&2; die "make failed (log: $work/build/make.log)"; }

    echo "spu-objdump: installing to $PREFIX"
    (cd "$work/build" && make install-binutils "${MAKE_ARGS[@]}" </dev/null >install.log 2>&1) ||
        { tail -n 30 "$work/build/install.log" >&2; die "make install failed (log: $work/build/install.log)"; }

    [ -x "$OBJDUMP" ] || die "the build did not produce $OBJDUMP"
    local version_line
    version_line="$("$OBJDUMP" --version </dev/null | head -n 1)"
    case "$version_line" in
        *" $BINUTILS_VERSION"*) ;;
        *) die "unexpected version from $OBJDUMP: $version_line" ;;
    esac

    # Written last: its presence means the install is complete.
    printf '%s\n' "$pin" >"$MARKER"
    echo "spu-objdump: ready $OBJDUMP"
}

# main reads no stdin: with `bash -s` the script itself arrives on stdin and a child process would consume it.
main "$@" </dev/null
exit $?
