# Pin of the FFmpeg build fetched by tools/fetch-ffmpeg.ps1 (LGPL, shared, 9.0 release branch).
# The script reads these three values by regex, so keep each one on its own `set(...)` line.
# BtbN may delete old autobuild releases; keep a copy of the zip and pass it with -From. When
# changing the pin, update THIRD_PARTY_NOTICES.md as well.
set(DAL_FFMPEG_RELEASE_TAG "autobuild-2026-10-06-13-06")
set(DAL_FFMPEG_ASSET "ffmpeg-n9.0.2-22-g46d8f462ee-win64-lgpl-shared-9.0.zip")
set(DAL_FFMPEG_SHA256 "c6a4d770f93248d62a3ea55b8fa42855f5a6adc078e5db370a51fc7f2487114e")
