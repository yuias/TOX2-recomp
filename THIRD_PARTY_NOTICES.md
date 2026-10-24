# Third-party notices

## Microsoft GameInput

On Windows, gamepads are read with Microsoft's GameInput API (version 3). On macOS this package is
not used: gamepads are read through Apple's GameController.framework, a system framework. The
`Microsoft.GameInput` NuGet package is not part of this repository. `tools/fetch-gameinput.ps1` downloads the pinned package
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

## NVIDIA Streamline and DLSS

On Windows, the optional DLAA anti-aliasing mode (`--aa-mode dlaa`) runs NVIDIA DLSS through the NVIDIA Streamline
SDK, <https://github.com/NVIDIA-RTX/Streamline>, and the optional frame generation (`--frame-gen`) runs NVIDIA DLSS
Frame Generation and NVIDIA Reflex through the same SDK. The macOS build does not use Streamline. The
SDK is not part of this repository: nothing from it is committed. `tools/fetch-streamline.ps1` downloads the pinned release into `externals/Streamline`
(git-ignored) and verifies its SHA-256 (`-From <zip>` takes a local copy and applies the same check);
the pin is in `cmake/streamline.cmake`. The build uses the SDK's headers at compile time and, when
the SDK is present, copies four DLLs (eight with frame generation) and the licence texts next to the
executables (`bin/` of the build directory, the licences under `bin/licenses/streamline/`).
`TOX2_ENABLE_DLSS=OFF` builds without any of this.

| | |
| --- | --- |
| Version | Streamline 2.14.1 (`streamline-sdk-v2.14.1.zip`, SHA-256 `92c4d954631a1710da86ca3fa8d5034f2b9503838c95fc4ae977ae149319781b`) |
| Source | <https://github.com/NVIDIA-RTX/Streamline/releases/download/v2.14.1/streamline-sdk-v2.14.1.zip> |
| Streamline itself: the headers (build time only) and `sl.interposer.dll`, `sl.common.dll`, `sl.dlss.dll`, `sl.reflex.dll`, `sl.pcl.dll` | MIT License, Copyright (c) 2023 NVIDIA CORPORATION (the SDK's `license.txt`, copied as `LICENSE.txt`) |
| Runtime DLLs copied next to the executables | `sl.interposer.dll`, `sl.common.dll`, `sl.dlss.dll`, `nvngx_dlss.dll`; for frame generation `sl.dlss_g.dll`, `sl.reflex.dll`, `sl.pcl.dll`, `nvngx_dlssg.dll` |
| DLSS Frame Generation: `sl.dlss_g.dll` (a prebuilt plugin; its source is not in the SDK) and `nvngx_dlssg.dll` | No licence text of their own ships with them in the SDK. The build only copies them from the SDK the developer downloaded into the local build output; this project does not distribute build output, so it does not distribute these DLLs |
| DLSS runtime and `nvngx_dlss.dll` | NVIDIA RTX SDKs LICENSE (v. March 14, 2024) with the NVIDIA RTX supplement (the SDK's `bin/x64/nvngx_dlss.license.txt`, copied as `nvngx_dlss.license.txt`) |
| SDK third-party components | `3rd-party-licenses.md` of the SDK (copied unchanged) |

### Licences

The text of each licence ships with the build in `bin/licenses/streamline/` and must accompany any
redistributed copy of the DLLs. A summary of what the texts say about redistribution follows; the files
themselves are authoritative:

- `license.txt` is the MIT License, with the condition that the copyright notice and the permission
  notice are included in all copies or substantial portions of the software. A note in the same file
  says that two Streamline files, `sl_nvperf.h` and `sl_nvperf.dll`, are covered by the NVIDIA Nsight
  Perf SDK License instead; this project does not use or copy either of them.
- `nvngx_dlss.license.txt` grants the right to distribute software and materials within the SDK,
  other than developer tools, as incorporated in object code format into a software application, subject
  to its distribution requirements. These include: the application must have material additional
  functionality beyond the included portions of the SDK; the SDK is to be distributed under terms at
  least as protective as that licence; and NVIDIA is to be notified in writing of any known or suspected
  non-compliant distribution or use. The supplement licenses the DLSS SDK for developing applications
  for use in systems with NVIDIA GPUs, asks for notification to NVIDIA before the commercial release of
  an application that incorporates it, requires applications that incorporate it to attribute the use of
  the SDK and include the NVIDIA Marks on splash screens, in the about box (if present) and in the
  credits of game applications, and states that NVIDIA may make over-the-air updates of the SDK.
  The licence forbids reverse engineering the SDK and bypassing or circumventing any security or
  authentication mechanism in it, and forbids using the SDK in a way that would make it subject to an
  open source software license (one that requires, as a condition of use, modification or distribution, that
  the SDK be disclosed or distributed in source code form, licensed for making derivative works, or
  redistributable at no charge).
- The SDK also ships `NvLowLatencyVk.dll` (NVIDIA Reflex), which `sl.common.dll` tries to load at
  start-up. It is not copied: its licence, `bin/x64/reflex.license.txt`, lists only "the NVAPIs, .lib
  and .h files, Linux wrapper, and samples" as distributable. On Vulkan, Streamline uses the driver's
  `VK_NV_low_latency2` extension for Reflex, so frame generation does not need the file.
  The Reflex SDK supplement in that file (v. December 9, 2020) also asks that a product attribute the use of the SDK with an
  NVIDIA Reflex reference for the features developed with it. Frame generation needs Reflex, and this project
  therefore names "NVIDIA Reflex" and "NVIDIA DLSS Frame Generation" in this file and in
  [Frame generation](docs/rendering.md#frame-generation). The same attribution requirements as for DLSS, in
  `nvngx_dlss.license.txt` above, apply to DLSS Frame Generation; the project has no splash screen or about box.
- `3rd-party-licenses.md` lists the third-party software that the SDK includes, with the licence of
  each.

### Run-time loading

Streamline is not linked into the executables, and no import library of the SDK is used. `tox2_game`,
`tox2_rsxreplay` and the render tests start without any of the DLLs. When DLAA is requested (and in
every `tox2_game` run that uses Vulkan, so that the overlay can offer it), the renderer loads
`sl.interposer.dll` from the directory of the executable with `LoadLibraryExW`, after
`sl::security::verifyEmbeddedSignature` from the SDK has accepted the file's NVIDIA signature. The DLL
is only ever loaded from there. Over-the-air updates and downloaded plugins are switched off in the
Streamline preferences, so only the DLLs next to the executable are used, and Streamline's log output
goes to the project log. If the DLLs are missing, unsigned, or the GPU or driver cannot run DLSS, DLAA is
unavailable and the renderer falls back to `msaa` (or `supersample`); see [Rendering](docs/rendering.md#dlaa).
Streamline loads the four frame generation DLLs from the same directory. If one is missing, frame generation is
unavailable and DLAA is not affected.

## RPCS3 key files

`tox2_decrypt` needs the keys that decrypt `EBOOT.BIN` and the firmware's `libsre.prx`. It reads
them from two source files of RPCS3, <https://github.com/RPCS3/rpcs3>, licensed under the GNU
General Public License, version 2:

| | |
| --- | --- |
| Commit | `f9b8c209a3800c1865bcfeade611f65f5237138c` |
| `rpcs3/Crypto/key_vault.h` | SHA-256 `d5d0cede74c7900bb23ca8d60066194648ed4b976eaee60315c0bcbed7dc2aaf` |
| `rpcs3/Crypto/key_vault.cpp` | SHA-256 `a2947ff6b681258c198552ec6344e16685e2df649018206daa3f41a0a1aadbd7` |

The files are downloaded at configuration time into `generated/deps/rpcs3/<commit>/`, only when
`-EbootBin` or `-Pup` is given, and their SHA-256 is checked (the pins are in
`cmake/rpcs3_keys.cmake`). Alternatively `TOX2_RPCS3_CRYPTO_DIR` (`-Rpcs3CryptoDir`) names the
`rpcs3/Crypto` directory of a local RPCS3 checkout. `tox2_decrypt` reads only the key constants from
the files, as data. They are not compiled, linked, committed or redistributed, and no key appears
in this repository or in any build output other than the decrypted files themselves.

## FFmpeg

The runtime decodes the movies (H.264 video and ATRAC3plus audio) and the game's ATRAC3plus sound
(music and sound effects) with FFmpeg (`libavcodec`, `libavutil`, `libswscale`; `libavcodec`
also imports `libswresample`, none of whose functions the runtime calls). FFmpeg is not part of
this repository and no FFmpeg binary is tracked. On Windows, `tools/fetch-ffmpeg.ps1` downloads a
pinned, SHA-256 verified build into `generated/deps/ffmpeg` (`-From <zip>` takes a local copy of the
pinned zip and applies the same check); the pin is in `cmake/ffmpeg.cmake`. The build copies the
four DLLs, the FFmpeg `LICENSE.txt` and this notice next to `tox2_game` (`bin/` of the build
directory, the licence and notice under `bin/licenses/ffmpeg/`). Unless it says macOS,
this section describes that Windows distribution; "FFmpeg on macOS" at its end covers the Mac.

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

### FFmpeg on macOS

On macOS the build uses no pinned FFmpeg and downloads none. The runtime loads the user's own
Homebrew FFmpeg (`brew install ffmpeg`) in place: `libavutil`, `libswresample`, `libswscale` and
`libavcodec` as `lib<name>.<major>.dylib`, opened with `dlopen` by absolute path from the Homebrew
`lib` directory that was compiled in. Nothing is linked, copied into the build output or
redistributed. Homebrew's FFmpeg is a GPL-configured build (it includes x264 and x265), unlike the
pinned LGPL build of the Windows distribution. This project loads it from the user's own
installation and never copies, bundles or redistributes it. The runtime uses only the H.264 and
ATRAC3plus decoders and the conversion functions of `libswscale`. See
[Movies](docs/movies.md#ffmpeg-on-macos).

## Dear ImGui

The in-window debug overlay (`F1`, see [Debug overlay](docs/runtime.md#debug-overlay)) is drawn with
Dear ImGui, <https://github.com/ocornut/imgui>. It is a git submodule (`externals/imgui`) and is
built from source as a static library into `tox2_game` and the overlay tests: the core files plus
the Vulkan backend and, on Windows, the Win32 backend or, on macOS, the Cocoa backend
(`imgui_impl_osx.mm`, compiled through `runtime/src/overlay/imgui_osx_bridge.mm`), without the
demo window. The Vulkan backend loads its entry points
through volk. The overlay uses ImGui's built-in default font, so no font file is involved.

| | |
| --- | --- |
| Version | Dear ImGui 1.91.9b (tag `v1.91.9b`, commit `f5befd2d29e66809cd1110a152e375a7f1981f06`) |
| Source | <https://github.com/ocornut/imgui/tree/v1.91.9b> |
| Licence | MIT License, Copyright (c) 2014-2025 Omar Cornut |

The licence text is also in `externals/imgui/LICENSE.txt`:

```
The MIT License (MIT)

Copyright (c) 2014-2025 Omar Cornut

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## spdlog

The runtime's log lines (see [Logging](docs/runtime.md#logging)) are written with spdlog,
<https://github.com/gabime/spdlog>. It is a git submodule (`externals/spdlog`) and is built from
source as a static library, not header-only, into `tox2_runtime_core`, which `tox2_game` and the
runtime test executables link. Only `runtime/src/core/log.cpp` includes its headers. The build sets
`SPDLOG_USE_STD_FORMAT`, so spdlog formats with the C++20 `std::format` and the fmt library that
the submodule bundles (`externals/spdlog/include/spdlog/fmt/bundled`, MIT License, its own licence
in `fmt.license.rst` there) is neither compiled nor linked.

| | |
| --- | --- |
| Version | spdlog 1.17.0 (tag `v1.17.0`, commit `79524ddd08a4ec981b7fea76afd08ee05f83755d`) |
| Source | <https://github.com/gabime/spdlog/tree/v1.17.0> |
| Licence | MIT License, Copyright (c) 2016 - present, Gabi Melman and spdlog contributors |

The licence text is also in `externals/spdlog/LICENSE`:

```
The MIT License (MIT)

Copyright (c) 2016 - present, Gabi Melman and spdlog contributors.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.

-- NOTE: Third party dependency used by this software --
This software depends on the fmt lib (MIT License),
and users must comply to its license: https://raw.githubusercontent.com/fmtlib/fmt/master/LICENSE
```

## yyjson

The runtime reads the JSON file of `--vfs-config` (`runtime/src/lv2/vfs_config.cpp`) and writes the
JSON file of `--rsx-census` (`runtime/src/rsx/rsx_census.cpp`) with yyjson,
<https://github.com/ibireme/yyjson>. It is a git submodule (`externals/yyjson`) and is built from
source as a static library into `tox2_runtime_core`, which `tox2_game` and the runtime test
executables link.

| | |
| --- | --- |
| Version | yyjson 0.13.0 (tag `0.13.0`, commit `6447536015f3d600f3d65323b10976103b337ca7`) |
| Source | <https://github.com/ibireme/yyjson/tree/0.13.0> |
| Licence | MIT License, Copyright (c) 2020 YaoYuan <ibireme@gmail.com> |

The licence text is also in `externals/yyjson/LICENSE`:

```
MIT License

Copyright (c) 2020 YaoYuan <ibireme@gmail.com>

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## xxHash

The RSX texture validation hashes the guest bytes of a texture with XXH3-64 from xxHash,
<https://github.com/Cyan4973/xxHash> (`rsxHashBytes`, `runtime/src/rsx/rsx_texture_cache.cpp`). It is
a git submodule (`externals/xxHash`) and is used header-only: `xxhash.h` is compiled into
`tox2_runtime_core` with `XXH_INLINE_ALL`, and no library is built from it. Only the library
(`xxhash.h`, `xxh3.h`) is used; the repository's command-line tool under `cli/` carries a different
licence (GPL v2) and is not built or used.

| | |
| --- | --- |
| Version | xxHash 0.8.4 (tag `v0.8.4`, commit `c87183a77d67f7d37e3d2d1b7eaac5e7c695e4f0`) |
| Source | <https://github.com/Cyan4973/xxHash/tree/v0.8.4> |
| Licence | BSD 2-Clause License, Copyright (c) 2012-2021 Yann Collet |

The licence text is also in `externals/xxHash/LICENSE`:

```
xxHash Library
Copyright (c) 2012-2021 Yann Collet
All rights reserved.

BSD 2-Clause License (https://www.opensource.org/licenses/bsd-license.php)

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

* Redistributions of source code must retain the above copyright notice, this
  list of conditions and the following disclaimer.

* Redistributions in binary form must reproduce the above copyright notice, this
  list of conditions and the following disclaimer in the documentation and/or
  other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

## zlib

zlib, <https://github.com/madler/zlib>, is used in two places. `tox2_decrypt` inflates the
compressed sections of the game's and the firmware's SELF files with it (`tools/tox2_decrypt`,
target `tox2_decrypt_lib`), and libpng needs it (see the next section). It is a git submodule
(`externals/zlib`) and is built from source as a static library (`zlibstatic`).

| | |
| --- | --- |
| Version | zlib 1.3.2 (tag `v1.3.2`, commit `da607da739fa6047df13e66a2af6b8bec7c2a498`) |
| Source | <https://github.com/madler/zlib/tree/v1.3.2> |
| Licence | zlib License, (C) 1995-2026 Jean-loup Gailly and Mark Adler |

The licence text is also in `externals/zlib/LICENSE`:

```
Copyright notice:

 (C) 1995-2026 Jean-loup Gailly and Mark Adler

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.

  Jean-loup Gailly        Mark Adler
  jloup@gzip.org          madler@alumni.caltech.edu
```

## libpng

libpng, <https://github.com/pnggroup/libpng>, decodes PNG images for the `cellPngDec` handlers
(`runtime/src/hle/cell_png_dec.cpp`, in `tox2_runtime_core`) and encodes the PNG files that
`--dump-frames` and `tox2_rsxreplay` write (`runtime/src/render/frame_dump.cpp`, in `tox2_render`).
The runtime tests also use its writer to build their inputs. It is a git submodule
(`externals/libpng`) and is built from source as a static library (`png_static`) against the zlib
above, with the library's prebuilt `pnglibconf.h`. Both libraries are linked into `tox2_game`
through `tox2_runtime_core` and `tox2_render`.

| | |
| --- | --- |
| Version | libpng 1.6.59 (tag `v1.6.59`, commit `cd952f49f95bb27154ae77dbb103032d95f6e580`) |
| Source | <https://github.com/pnggroup/libpng/tree/v1.6.59> |
| Licence | PNG Reference Library License version 2, Copyright (c) 1995-2026 The PNG Reference Library Authors |

The licence text is also in `externals/libpng/LICENSE`. It lists the earlier copyright holders and
also contains the version 1 licence of libpng 0.5 through 1.6.35:

```
COPYRIGHT NOTICE, DISCLAIMER, and LICENSE
=========================================

PNG Reference Library License version 2
---------------------------------------

 * Copyright (c) 1995-2026 The PNG Reference Library Authors.
 * Copyright (c) 2018-2026 Cosmin Truta.
 * Copyright (c) 2000-2002, 2004, 2006-2018 Glenn Randers-Pehrson.
 * Copyright (c) 1996-1997 Andreas Dilger.
 * Copyright (c) 1995-1996 Guy Eric Schalnat, Group 42, Inc.

The software is supplied "as is", without warranty of any kind,
express or implied, including, without limitation, the warranties
of merchantability, fitness for a particular purpose, title, and
non-infringement.  In no event shall the Copyright owners, or
anyone distributing the software, be liable for any damages or
other liability, whether in contract, tort or otherwise, arising
from, out of, or in connection with the software, or the use or
other dealings in the software, even if advised of the possibility
of such damage.

Permission is hereby granted to use, copy, modify, and distribute
this software, or portions hereof, for any purpose, without fee,
subject to the following restrictions:

 1. The origin of this software must not be misrepresented; you
    must not claim that you wrote the original software.  If you
    use this software in a product, an acknowledgment in the product
    documentation would be appreciated, but is not required.

 2. Altered source versions must be plainly marked as such, and must
    not be misrepresented as being the original software.

 3. This Copyright notice may not be removed or altered from any
    source or altered source distribution.


PNG Reference Library License version 1 (for libpng 0.5 through 1.6.35)
-----------------------------------------------------------------------

libpng versions 1.0.7, July 1, 2000, through 1.6.35, July 15, 2018 are
Copyright (c) 2000-2002, 2004, 2006-2018 Glenn Randers-Pehrson, are
derived from libpng-1.0.6, and are distributed according to the same
disclaimer and license as libpng-1.0.6 with the following individuals
added to the list of Contributing Authors:

    Simon-Pierre Cadieux
    Eric S. Raymond
    Mans Rullgard
    Cosmin Truta
    Gilles Vollant
    James Yu
    Mandar Sahastrabuddhe
    Google Inc.
    Vadim Barkov

and with the following additions to the disclaimer:

    There is no warranty against interference with your enjoyment of
    the library or against infringement.  There is no warranty that our
    efforts or the library will fulfill any of your particular purposes
    or needs.  This library is provided with all faults, and the entire
    risk of satisfactory quality, performance, accuracy, and effort is
    with the user.

Some files in the "contrib" directory and some configure-generated
files that are distributed with libpng have other copyright owners, and
are released under other open source licenses.

libpng versions 0.97, January 1998, through 1.0.6, March 20, 2000, are
Copyright (c) 1998-2000 Glenn Randers-Pehrson, are derived from
libpng-0.96, and are distributed according to the same disclaimer and
license as libpng-0.96, with the following individuals added to the
list of Contributing Authors:

    Tom Lane
    Glenn Randers-Pehrson
    Willem van Schaik

libpng versions 0.89, June 1996, through 0.96, May 1997, are
Copyright (c) 1996-1997 Andreas Dilger, are derived from libpng-0.88,
and are distributed according to the same disclaimer and license as
libpng-0.88, with the following individuals added to the list of
Contributing Authors:

    John Bowler
    Kevin Bracey
    Sam Bushell
    Magnus Holmgren
    Greg Roelofs
    Tom Tanner

Some files in the "scripts" directory have other copyright owners,
but are released under this license.

libpng versions 0.5, May 1995, through 0.88, January 1996, are
Copyright (c) 1995-1996 Guy Eric Schalnat, Group 42, Inc.

For the purposes of this copyright and license, "Contributing Authors"
is defined as the following set of individuals:

    Andreas Dilger
    Dave Martindale
    Guy Eric Schalnat
    Paul Schmidt
    Tim Wegner

The PNG Reference Library is supplied "AS IS".  The Contributing
Authors and Group 42, Inc. disclaim all warranties, expressed or
implied, including, without limitation, the warranties of
merchantability and of fitness for any purpose.  The Contributing
Authors and Group 42, Inc. assume no liability for direct, indirect,
incidental, special, exemplary, or consequential damages, which may
result from the use of the PNG Reference Library, even if advised of
the possibility of such damage.

Permission is hereby granted to use, copy, modify, and distribute this
source code, or portions hereof, for any purpose, without fee, subject
to the following restrictions:

 1. The origin of this source code must not be misrepresented.

 2. Altered versions must be plainly marked as such and must not
    be misrepresented as being the original source.

 3. This Copyright notice may not be removed or altered from any
    source or altered source distribution.

The Contributing Authors and Group 42, Inc. specifically permit,
without fee, and encourage the use of this source code as a component
to supporting the PNG file format in commercial products.  If you use
this source code in a product, acknowledgment is not required but would
be appreciated.
```

## Vulkan-Headers

The renderer and the debug overlay compile against the Vulkan headers of the Khronos
Vulkan-Headers repository, <https://github.com/KhronosGroup/Vulkan-Headers>, so the headers do not
depend on the Vulkan SDK installed on a machine. It is a git submodule (`externals/Vulkan-Headers`).
Only its `include` directory is used (`vulkan/vulkan.h` and the headers it includes, including
`vulkan_win32.h`, and `vk_video`); it is added to the include path of the `volk` target, so every
target that links volk sees it. Nothing is built from it.

| | |
| --- | --- |
| Version | Vulkan-Headers 1.4.363 (tag `v1.4.363`, commit `6802bb4733b63ed5efd3adb308a6c885ef180ea1`) |
| Source | <https://github.com/KhronosGroup/Vulkan-Headers/tree/v1.4.363> |
| Licence | Apache License 2.0 or MIT License (`SPDX-License-Identifier: Apache-2.0 OR MIT`); this project uses the MIT License, Copyright (c) 2015-2023 The Khronos Group Inc. |

The repository has a `LICENSES/` directory with the texts of both licences (`Apache-2.0.txt` and
`MIT.txt`). Every header under `include/` carries `SPDX-License-Identifier: Apache-2.0 OR MIT` and
the line `Copyright 2015-2026 The Khronos Group Inc.`, so a recipient may use them under either
licence. This project uses them under the MIT License, whose text is `externals/Vulkan-Headers/LICENSES/MIT.txt`:

```
MIT License

Copyright (c) 2015-2023 The Khronos Group Inc.

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
```

## volk

The renderer and the debug overlay load the Vulkan entry points at run time through volk,
<https://github.com/zeux/volk>, so no Vulkan import library is linked. It is a git submodule
(`externals/volk`); its single source file `volk.c` is built into a static library (`volk`) that
`tox2_render` and the Dear ImGui library link, and so `tox2_game` and `tox2_rsxreplay`.

| | |
| --- | --- |
| Version | volk 1.4.363 (tag `vulkan-sdk-1.4.363.0`, commit `54fc0d7a72887cbfc5b030d26897b2b40ac53fb8`) |
| Source | <https://github.com/zeux/volk/tree/vulkan-sdk-1.4.363.0> |
| Licence | MIT License, Copyright (c) 2018-2026 Arseny Kapoulkine |

The licence text is also in `externals/volk/LICENSE.md`:

```
Copyright (c) 2018-2026 Arseny Kapoulkine

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## VulkanMemoryAllocator

The renderer allocates the memory of its Vulkan buffers and images with the Vulkan Memory Allocator
(VMA), <https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator>. It is a git submodule
(`externals/VulkanMemoryAllocator`) and only its single header `include/vk_mem_alloc.h` is used. It
is compiled into `tox2_render` (`runtime/src/render/vk_context.cpp` defines `VMA_IMPLEMENTATION`),
which `tox2_game` and `tox2_rsxreplay` link, and it resolves its Vulkan entry points through volk.

| | |
| --- | --- |
| Version | VulkanMemoryAllocator 3.4.0 (tag `v3.4.0`, commit `3aa921224c154a0d2c43912bc88e1c42ce1f7607`) |
| Source | <https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator/tree/v3.4.0> |
| Licence | MIT License, Copyright (c) 2017-2026 Advanced Micro Devices, Inc. |

The licence text is also in `externals/VulkanMemoryAllocator/LICENSE.txt`:

```
Copyright (c) 2017-2026 Advanced Micro Devices, Inc. All rights reserved.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
```

## BIZ UDPGothic

The save dialog draws its text with the firmware's Japanese font when the firmware PUP is configured, and
otherwise with BIZ UDPGothic Regular, <https://github.com/googlefonts/morisawa-biz-ud-gothic>. The font is not
committed: configuration downloads it into `generated/deps/fonts/<commit>/` and checks its SHA-256 (the pins are
in `cmake/ui_font.cmake`), and the build copies it to `bin/fonts/` with its licence text in
`bin/licenses/biz-udgothic/OFL.txt`. See [Building](docs/building.md#fallback-ui-font).

| | |
| --- | --- |
| Commit | `18934af56b9c003ca58c54bffbf226848cb11032` |
| `fonts/ttf/BIZUDPGothic-Regular.ttf` | SHA-256 `258d7156c165f2ff774b6efee637c22c3b950de0d8a10e501137061bc8085d01` |
| `OFL.txt` | SHA-256 `e753d7155d53c747d037a445e584c8ecfca6dd79846db610417e282a736b28bc` |
| Licence | SIL Open Font License, Version 1.1, Copyright 2022 The BIZ UDGothic Project Authors (<https://github.com/googlefonts/morisawa-biz-ud-mincho>). The licence text ships with the font as `OFL.txt` and must accompany any redistributed copy of the font |
