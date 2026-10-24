#!/bin/sh
# Configures and builds one CMake preset and optionally runs its tests: the counterpart of
# tools/build.ps1. The configure step writes bin/config.json (cmake/GameConfig.cmake).
#
# usage: tools/build.sh [--preset <name>] [--clean] [--test] [--eboot-elf <file>]
#        [--generated-dir <dir>] [--game-dir <dir>] [--eboot-bin <file>] [--pup <file>]
#        [--rpcs3-crypto-dir <dir>] [--llvm-prefix <dir>]
#
# The preset defaults to macos-debug on Darwin and is required elsewhere. --llvm-prefix is exported
# as TOX2_LLVM_PREFIX for the toolchain file.
#
# Exit codes: the exit code of the failing command, 1 for a failed check of this script, 2 for a
# usage error.
set -eu
unset CDPATH

usage() {
    cat >&2 <<'EOF'
usage: tools/build.sh [--preset <name>] [--clean] [--test] [--eboot-elf <file>]
                      [--generated-dir <dir>] [--game-dir <dir>] [--eboot-bin <file>] [--pup <file>]
                      [--rpcs3-crypto-dir <dir>] [--llvm-prefix <dir>]
EOF
}

usage_error() {
    echo "build: $1" >&2
    usage
    exit 2
}

fail() {
    echo "build: $1" >&2
    exit 1
}

# Lexical absolute path against the caller's directory; trailing separators are not significant.
abs_lexical() {
    case $1 in
        /*) p=$1 ;;
        *) p=$PWD/$1 ;;
    esac
    while [ "$p" != / ] && [ "${p%/}" != "$p" ]; do
        p=${p%/}
    done
    printf '%s\n' "$p"
}

# Absolute path of a directory that may not exist (CMake reports a missing one with the command to
# run); symbolic links are kept.
abs_dir() {
    if [ -d "$1" ]; then
        case $1 in
            /*) d=$1 ;;
            *) d=./$1 ;;
        esac
        (cd "$d" && pwd)
    else
        abs_lexical "$1"
    fi
}

# Absolute path of an existing file or directory.
abs_existing() {
    [ -e "$2" ] || fail "$1 not found: $2"
    if [ -d "$2" ]; then
        abs_dir "$2"
    else
        parent=$(dirname "$2")
        printf '%s/%s\n' "$(abs_dir "$parent")" "$(basename "$2")"
    fi
}

# Seconds since the epoch with a fraction when python3 is available, whole seconds otherwise.
now() {
    python3 -I -c 'import time; print("%.3f" % time.time())' 2>/dev/null || date +%s
}

preset=
clean=0
run_tests=0
eboot_elf=
generated_dir=
game_dir=
eboot_bin=
pup=
rpcs3_crypto_dir=
llvm_prefix=${TOX2_LLVM_PREFIX:-}
while [ $# -gt 0 ]; do
    case $1 in
        --clean)
            clean=1
            shift
            ;;
        --test)
            run_tests=1
            shift
            ;;
        --preset | --eboot-elf | --generated-dir | --game-dir | --eboot-bin | --pup | --rpcs3-crypto-dir | \
            --llvm-prefix)
            [ $# -ge 2 ] || usage_error "$1 needs a value"
            case $1 in
                --preset) preset=$2 ;;
                --eboot-elf) eboot_elf=$2 ;;
                --generated-dir) generated_dir=$2 ;;
                --game-dir) game_dir=$2 ;;
                --eboot-bin) eboot_bin=$2 ;;
                --pup) pup=$2 ;;
                --rpcs3-crypto-dir) rpcs3_crypto_dir=$2 ;;
                --llvm-prefix) llvm_prefix=$2 ;;
            esac
            shift 2
            ;;
        *) usage_error "unexpected argument: $1" ;;
    esac
done

if [ -z "$preset" ]; then
    if [ "$(uname -s)" = Darwin ]; then
        preset=macos-debug
    else
        usage_error "--preset is required"
    fi
fi
case $preset in
    */* | .*) usage_error "invalid preset name: $preset" ;;
esac

# On macOS the toolchain file asks Homebrew when no prefix is given; ask the same way so the version
# printed below is the compiler that will be used.
if [ -z "$llvm_prefix" ] && [ "$(uname -s)" = Darwin ] && command -v brew >/dev/null 2>&1; then
    llvm_prefix=$(brew --prefix llvm 2>/dev/null || :)
    [ -x "$llvm_prefix/bin/clang" ] || llvm_prefix=
fi
clang=clang
if [ -n "$llvm_prefix" ]; then
    # Absolute, because the script changes into the repository root before CMake runs.
    llvm_prefix=$(abs_dir "$llvm_prefix")
    clang=$llvm_prefix/bin/clang
    TOX2_LLVM_PREFIX=$llvm_prefix
    export TOX2_LLVM_PREFIX
fi

# Captured fully: a pipe would hide the exit code of clang.
clang_version=$("$clang" --version) || exit $?
printf '%s\n' "$clang_version" | head -n 1

# Resolved against the caller's location, before changing into the repository root. The directories
# need not exist: CMake reports a missing one with the emit command to run.
[ -z "$generated_dir" ] || generated_dir=$(abs_dir "$generated_dir")
[ -z "$game_dir" ] || game_dir=$(abs_dir "$game_dir")
# These must exist.
[ -z "$eboot_elf" ] || eboot_elf=$(abs_existing --eboot-elf "$eboot_elf")
[ -z "$eboot_bin" ] || eboot_bin=$(abs_existing --eboot-bin "$eboot_bin")
[ -z "$pup" ] || pup=$(abs_existing --pup "$pup")
[ -z "$rpcs3_crypto_dir" ] || rpcs3_crypto_dir=$(abs_existing --rpcs3-crypto-dir "$rpcs3_crypto_dir")

script_dir=$(cd "$(dirname "$0")" && pwd)
repo_root=$(cd "$script_dir/.." && pwd)
cd "$repo_root"

if [ "$clean" = 1 ]; then
    rm -rf "build/$preset"
fi

set -- --preset "$preset"
[ -z "$eboot_elf" ] || set -- "$@" "-DTOX2_EBOOT_ELF=$eboot_elf"
[ -z "$generated_dir" ] || set -- "$@" "-DTOX2_PPU_GENERATED_DIR=$generated_dir"
[ -z "$game_dir" ] || set -- "$@" "-DTOX2_GAME_DIR=$game_dir"
[ -z "$eboot_bin" ] || set -- "$@" "-DTOX2_EBOOT_BIN=$eboot_bin"
[ -z "$pup" ] || set -- "$@" "-DTOX2_PUP=$pup"
[ -z "$rpcs3_crypto_dir" ] || set -- "$@" "-DTOX2_RPCS3_CRYPTO_DIR=$rpcs3_crypto_dir"
cmake "$@"

build_start=$(now)
build_exit=0
cmake --build --preset "$preset" || build_exit=$?
build_end=$(now)
echo "build: $(awk -v start="$build_start" -v end="$build_end" 'BEGIN { printf "%.1f", end - start }') s"
[ "$build_exit" -eq 0 ] || exit "$build_exit"

if [ "$run_tests" = 1 ]; then
    ctest --preset "$preset"
fi
