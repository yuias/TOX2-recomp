# Pin of the Microsoft.GameInput NuGet package fetched by tools/fetch-gameinput.ps1 (API version 3).
# The script reads these two values by regex, so keep each one on its own `set(...)` line.
# The SHA-256 is of the .nupkg as served by api.nuget.org. It was computed locally and cross-checked
# against the SHA-512 `packageHash` that nuget.org publishes for this version in its catalog entry
# (https://api.nuget.org/v3/catalog0/data/2026.09.28.23.41.51/microsoft.gameinput.3.5.283.json).
# The runtime is not used from the package: the user installs redist/GameInputRedist.msi, because a
# copy of its DLL next to the executable hides Xbox pads.
# When changing the pin, update THIRD_PARTY_NOTICES.md as well.
set(TOX2_GAMEINPUT_VERSION "3.5.283")
set(TOX2_GAMEINPUT_SHA256 "b5988cb8ff9d7009b6ddf6ad4e3ff87e91b00cc17ffcd5d628dabc21c208f100")
