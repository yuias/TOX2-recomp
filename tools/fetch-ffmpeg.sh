#!/bin/sh
# Fetches the pinned FFmpeg build (cmake/ffmpeg.cmake) into generated/deps/ffmpeg.
#
# Downloads the pinned zip, verifies its SHA-256, extracts it into generated/deps/ffmpeg/<asset
# stem>/ and writes a .verified-sha256 marker there. A second run is a no-op. The pin is the Windows
# build: this script is for fetching it from a POSIX host, the macOS build uses Homebrew's FFmpeg.
#
# usage: tools/fetch-ffmpeg.sh [--from <zip>] [--force]
#
#   --from <zip>  Use this local zip instead of downloading (offline use, or a kept copy of the
#                 pinned build). It gets the same SHA-256 check and is never modified or deleted.
#   --force       Discard an existing extraction and extract again.
#
# TOX2_FETCH_FFMPEG_ROOT replaces the repository root (pin file and output directory); it exists for
# the tests only.
#
# Exit codes: 0 success (including no-op), 1 failure, 2 usage error.
set -eu
unset CDPATH

usage() {
    echo "usage: tools/fetch-ffmpeg.sh [--from <zip>] [--force]" >&2
}

usage_error() {
    echo "fetch-ffmpeg: $1" >&2
    usage
    exit 2
}

die() {
    echo "fetch-ffmpeg: $1" >&2
    exit 1
}

# Lower-case SHA-256 of a file.
sha256() {
    if command -v shasum >/dev/null 2>&1; then
        shasum -a 256 "$1" | awk '{ print tolower($1) }'
    elif command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | awk '{ print tolower($1) }'
    else
        die "neither shasum nor sha256sum is available"
    fi
}

# read_pin NAME PATTERN: the quoted value of `set(NAME "value")` in the pin file; PATTERN is a
# basic regular expression for the value.
read_pin() {
    value=$(sed -n "s/^.*set([[:space:]]*$1[[:space:]][[:space:]]*\"\\($2\\)\"[[:space:]]*).*\$/\\1/p" \
        "$pin_file" | head -n 1)
    [ -n "$value" ] || die "$1 is missing or malformed in cmake/ffmpeg.cmake"
    printf '%s\n' "$value"
}

# Rejects every entry that could land outside the destination, then extracts the archive.
expand_zip_safely() {
    zip_file=$1
    destination=$2
    unzip -Z1 "$zip_file" >"$destination.list" || die "cannot read the archive: $zip_file"
    while IFS= read -r name; do
        # Both separators count; a backslash would be a path separator for Windows tools.
        normalized=$(printf '%s' "$name" | tr '\\' '/')
        case $normalized in
            /* | *:*) die "unsafe entry in the archive: '$name'" ;;
        esac
        case /$normalized/ in
            */../*) die "unsafe entry in the archive: '$name'" ;;
        esac
    done <"$destination.list"
    rm -f "$destination.list"
    unzip -q -o "$zip_file" -d "$destination" || die "cannot extract the archive: $zip_file"
    # A link could point anywhere; the Windows build of the script extracts regular files only.
    if [ -n "$(find "$destination" -type l | head -n 1)" ]; then
        die "the archive contains a symbolic link"
    fi
}

from=
have_from=0
force=0
unexpected=
while [ $# -gt 0 ]; do
    case $1 in
        --force)
            force=1
            shift
            ;;
        --from)
            have_from=1
            if [ $# -ge 2 ]; then
                from=$2
                shift 2
            else
                shift
            fi
            ;;
        *)
            unexpected=${unexpected:+$unexpected }$1
            shift
            ;;
    esac
done
if [ -n "$unexpected" ]; then
    usage_error "unexpected argument: $unexpected"
fi
if [ "$have_from" = 1 ] && [ -z "$(printf '%s' "$from" | tr -d ' \t')" ]; then
    usage_error "--from needs a path"
fi

# Resolved against the caller's location, before anything else.
zip_path=
if [ -n "$from" ]; then
    case $from in
        /*) zip_path=$from ;;
        *) zip_path=$PWD/$from ;;
    esac
fi

staging=
cleanup() {
    if [ -n "$staging" ] && [ -e "$staging" ]; then
        rm -rf "$staging" "$staging.list"
    fi
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

if [ -n "${TOX2_FETCH_FFMPEG_ROOT:-}" ]; then
    repo_root=$TOX2_FETCH_FFMPEG_ROOT
else
    repo_root=$(cd "$(dirname "$0")/.." && pwd)
fi
pin_file=$repo_root/cmake/ffmpeg.cmake
[ -f "$pin_file" ] || die "pin file not found: cmake/ffmpeg.cmake"
tag=$(read_pin TOX2_FFMPEG_RELEASE_TAG 'autobuild-[0-9-][0-9-]*') || exit 1
asset=$(read_pin TOX2_FFMPEG_ASSET '[A-Za-z0-9._-][A-Za-z0-9._-]*\.zip') || exit 1
expected_hash=$(read_pin TOX2_FFMPEG_SHA256 '[0-9A-Fa-f]\{64\}') || exit 1
expected_hash=$(printf '%s' "$expected_hash" | tr 'A-F' 'a-f')
stem=${asset%.zip}

deps_dir=$repo_root/generated/deps/ffmpeg
download_dir=$deps_dir/download
final_dir=$deps_dir/$stem
marker_file=$final_dir/.verified-sha256

# Verified before anything on disk is touched.
if [ -n "$from" ]; then
    [ -f "$zip_path" ] || die "--from file not found: $from"
    actual=$(sha256 "$zip_path") || exit 1
    if [ "$actual" != "$expected_hash" ]; then
        die "SHA-256 mismatch for --from file '$from': expected $expected_hash, got $actual"
    fi
fi

if [ "$force" = 0 ] && [ -f "$marker_file" ] &&
    [ "$(tr -d ' \t\r\n' <"$marker_file")" = "$expected_hash" ]; then
    echo "ffmpeg: up to date ($stem)"
    exit 0
fi

if [ -z "$zip_path" ]; then
    zip_path=$download_dir/$asset
    # A cached download is reused only if it still matches the pin.
    if [ -f "$zip_path" ] && [ "$(sha256 "$zip_path")" != "$expected_hash" ]; then
        rm -f "$zip_path"
    fi
    if [ ! -f "$zip_path" ]; then
        url=https://github.com/BtbN/FFmpeg-Builds/releases/download/$tag/$asset
        mkdir -p "$download_dir"
        part_path=$zip_path.part
        echo "ffmpeg: downloading $url"
        if ! curl_error=$(curl --fail --location --silent --show-error --output "$part_path" "$url" 2>&1); then
            rm -f "$part_path"
            die "download failed: $curl_error
  The pinned release may have been removed; use --from with a copy of the zip."
        fi
        actual=$(sha256 "$part_path") || exit 1
        if [ "$actual" != "$expected_hash" ]; then
            rm -f "$part_path"
            die "SHA-256 mismatch for the download: expected $expected_hash, got $actual"
        fi
        mv -f "$part_path" "$zip_path"
    fi
fi

# Extract beside the final location, then move into place, so a failure leaves no partial tree.
mkdir -p "$deps_dir"
staging=$deps_dir/.staging-$$
rm -rf "$staging"
mkdir -p "$staging"
echo "ffmpeg: extracting $stem"
expand_zip_safely "$zip_path" "$staging"

extracted=$staging/$stem
[ -d "$extracted/bin" ] || die "the archive does not contain '$stem/bin'"
rm -rf "$final_dir"
mv "$extracted" "$final_dir"
printf '%s' "$expected_hash" >"$marker_file"
echo "ffmpeg: ready in generated/deps/ffmpeg/$stem"
exit 0
