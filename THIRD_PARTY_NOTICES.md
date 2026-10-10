# Third-party notices

## Microsoft GameInput

Gamepads are read with Microsoft's GameInput API (version 3). The `Microsoft.GameInput` NuGet
package is not part of this repository. `tools/fetch-gameinput.ps1` downloads the pinned package
from <https://www.nuget.org/packages/Microsoft.GameInput> into `generated/deps/gameinput` and
verifies its SHA-256 (`-From <nupkg>` takes a local copy and applies the same check); the pin is
in `cmake/gameinput.cmake`. The build uses only the package's `GameInput.h` and `GameInput.lib`;
nothing from the package is copied into the build output. The runtime is Microsoft's GameInput
redistributable, which the user installs from the package's `redist/GameInputRedist.msi`.

| | |
| --- | --- |
| Version | `Microsoft.GameInput` 3.5.283 (API version 3) |
| Source | <https://api.nuget.org/v3-flatcontainer/microsoft.gameinput/3.5.283/microsoft.gameinput.3.5.283.nupkg>; project page <https://aka.ms/gameinput> |
| Header and static library | MIT License, Copyright (c) Microsoft Corporation (`GameInput.h`, `GameInput.lib`, and `GameInput.cpp`, the source of the library) |
| Redistributable runtime | Microsoft Software License Terms, "Microsoft GameInput Redistributable" (the package's `LICENSE.txt`) |

### Licences

The public header and the static library are licensed under the MIT License; the text is in the
header and in `GameInput.cpp`. The package's `LICENSE.txt` is the licence of the redistributable
runtime (`GameInputRedist.dll`, `GameInputRedist.msi`), and `NOTICE.txt` lists the third-party
component that the package incorporates (Fusion, MIT License). This project neither copies nor
redistributes the runtime.

### Run-time loading

`GameInput.lib` is a static loader: it links no GameInput DLL, so the executables start on a machine
without a GameInput runtime. At the first GameInput call it uses the runtime installed on the
machine (the in-box `GameInput.dll` or the installed `GameInputRedist.dll`). If no runtime that
implements API version 3 is found, `tox2_game` logs one warning that names `GameInputRedist.msi` and
the gamepads are disabled; the keyboard keeps working.

## FFmpeg

The runtime decodes the movies (H.264 video and ATRAC3plus audio) and the game's ATRAC3plus sound
(music and sound effects) with FFmpeg (`libavcodec`, `libavutil`, `libswscale`; `libavcodec`
also imports `libswresample`, none of whose functions the runtime calls). FFmpeg is not part of
this repository and no FFmpeg binary is tracked. `tools/fetch-ffmpeg.ps1` downloads a pinned,
SHA-256 verified build into `generated/deps/ffmpeg` (`-From <zip>` takes a local copy of the
pinned zip and applies the same check); the pin is in `cmake/ffmpeg.cmake`. The build copies the
four DLLs, the FFmpeg `LICENSE.txt` and this notice next to `tox2_game` (`bin/` of the build
directory, the licence and notice under `bin/licenses/ffmpeg/`).

| | |
| --- | --- |
| Version | FFmpeg `n9.0.2-22-g46d8f462ee` (release/9.0 branch, 22 commits after tag `n9.0.2`) |
| Build | BtbN FFmpeg-Builds, `win64-lgpl-shared`, release `autobuild-2026-10-06-13-06` |
| Licence | GNU Lesser General Public License, version 3 or later (see below) |

### Licence

FFmpeg is licensed under the GNU Lesser General Public License, version 2.1 or later. This build
is configured with `--enable-version3`, so the resulting libraries are distributed under the LGPL
version 3 or later. It is not configured with `--enable-gpl` or `--enable-nonfree`, and no GPL or
non-free component is included. The full licence text ships with the build as `LICENSE.txt` and must
accompany any redistributed copy of the DLLs.

### Dynamic loading and replacement

The FFmpeg DLLs are not linked into the executables. The runtime loads them at run time with
`LoadLibraryExW` and the flags `LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32`,
from the directory of the executable, so their dependencies are looked up only in that directory
and in the Windows system directory (not in `PATH` or the working directory). They can be replaced
by another build of the same major versions (`avcodec-63`, `avutil-61`, `swresample-7`,
`swscale-10`) without rebuilding this project; a different major version is reported as
unavailable. Without usable DLLs the runtime logs two warnings,
`tox2: sail: codecs unavailable: <reason>` and `tox2: atrac: no decoder: <reason>`. Every movie then
plays as black frames without sound for the duration of its stream and ends normally, and the
game's music and sound effects (`cellAtrac`) are silent.

### Source code

- FFmpeg source at the exact commit of the build:
  <https://github.com/FFmpeg/FFmpeg/tree/46d8f462eeb87ee1f704d8c44a0ee24fca471ad1>
  (commit `46d8f462eeb87ee1f704d8c44a0ee24fca471ad1`).
- Build scripts (toolchain, patches and the configuration of every bundled library) at the tag of
  the pinned release:
  <https://github.com/BtbN/FFmpeg-Builds/tree/autobuild-2026-10-06-13-06>.
- The pinned release: <https://github.com/BtbN/FFmpeg-Builds/releases/tag/autobuild-2026-10-06-13-06>.

### Libraries bundled in the build

The `ffmpeg -version` configuration of the pinned build enables these external components, which
are statically included in the DLLs under their own licences (see the build scripts above): iconv,
zlib, libxml2, libvmaf, fontconfig, harfbuzz, freetype, fribidi, lcms2, Vulkan, dav1d, vorbis,
rav1e, librsvg, gmp, lzma, liblcevc-dec, OpenCL, AMF, aom, chromaprint, ffnvcodec, libgme,
kvazaar, aribb24, aribcaption, libass, libbluray, libjxl, mp3lame, opus, libplacebo, librist,
libssh, theora, vpx, webp, zmq, lv2, libvpl, OpenAL, liboapv, opencore-amr (narrowband and
wideband), openh264, openjpeg, openmpt, schannel, SDL2, snappy, soxr, srt, svtav1, twolame, uavs3d,
VA-API, VapourSynth, vvenc, zimg and zvbi. This project uses only the H.264 and ATRAC3plus decoders
and the colour conversion of `libswscale`.
