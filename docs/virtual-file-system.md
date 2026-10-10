# Virtual file system

Guest paths map to host directories (`runtime/src/lv2/vfs.cpp`, following RPCS3's mount table):

| Guest path | Host directory | Access |
| --- | --- | --- |
| `/dev_bdvd`, `/app_home` | the `--game-dir` directory | read-only |
| `/dev_hdd0` | `<--hdd-dir>/dev_hdd0` | read and write |
| `/dev_hdd1` | `<--hdd-dir>/dev_hdd1` | read and write |

The table is the layout the command-line options produce. A VFS configuration file can change it
(other host directories, more devices, single-file overrides).

When neither an option nor the configuration file sets them, `tox2_game` uses defaults relative to
its own build directory (`build/<preset>`, the parent of the `bin` directory that holds the
executable): `/dev_hdd0` and `/dev_hdd1` go to `build/<preset>/hdd`, and the sdata cache is
`build/<preset>/sdata_cache` if that directory exists. The sdata cache is inherited from the earlier
game: this game has no `.sdat` files and nothing in the repository creates the directory. The log
reports each default it applies (`tox2: vfs: ... (default)`).

`..` components may not leave a device, and a guest cannot create entries directly under
`/dev_hdd0` or `/dev_hdd0/game`. The directories that the console's system software keeps on hdd0
(`game`, `home/00000001/savedata`, `home/00000001/trophy` and a few more) are created when
`/dev_hdd0` is mounted writable (`--hdd-dir` does). Host names are matched by the host file system, which is case-insensitive on
Windows. The `sys_fs_*` system calls and the `cellFs*` imports share one object table. The
virtual root lists the mounted devices.

## Paths without a device

A guest path that starts with something other than a mounted device resolves to nothing, and the
`sys_fs` and `cellFs` calls fail with `CELL_ENOTMOUNTED` (`0x8001003a`); a missing file on a mounted
device fails with `CELL_ENOENT`. The first occurrence of each operation, path and result is logged
as `tox2: fs: <operation> "<path>" ok` or, at warning level, `... failed 0x<code>`. This game's
first file system calls show it: it stats `/dev_bdvd/PS3_GAME/USRDIR/TLFILE.TLDAT`,
`/dev_bdvd/PS3_GAME/USRDIR/FILEHEADER.TOFHDB` and `/dev_bdvd/PS3_GAME/ICON0.PNG`, built on the
paths that `cellGameBootCheck` and `cellGameContentPermit` return (see [Status](status.md)). On an
empty hdd directory the game then creates game data under `/dev_hdd0/game/_GDATA_<number>/USRDIR`
and copies those files into it.

## Large files

The file calls use 64-bit offsets: `sys_fs_lseek`, `cellFsReadWithOffset` and
`cellFsWriteWithOffset` move the host file pointer with `SetFilePointerEx`, and an offset beyond
the largest signed 64-bit value is treated as past any file. So an offset above 2 GiB is supported
by the code. `TLFILE.TLDAT` is about 3 GB, and the game's install step has read all of it with the
plain `cellFsRead` import (which calls the handler of system call 802, like `cellFsWrite` and
`cellFsLseek` with 803 and 818), so sequential positions above 2 GiB have been exercised. No run has
yet called `cellFsLseek` or `cellFsReadWithOffset` with such an offset on the real file.

## VFS configuration file

`--vfs-config FILE` reads a JSON file (strict JSON, no comments) that replaces the fixed layout
above. `config/vfs.example.json` is a tracked example with placeholder paths; keep real
configurations out of the repository (`*.local.json` is git-ignored, for example
`config/vfs.local.json`).

```json
{
  "version": 1,
  "game_dir": "/path/to/game",
  "hdd_dir": "/path/to/hdd",
  "sdata_cache": "/path/to/sdata_cache",
  "mounts": {
    "/dev_usb000": {"host": "usb_stick", "read_only": false, "block_size": 4096}
  },
  "files": {
    "/dev_bdvd/PS3_GAME/USRDIR/example.sdat": "/path/to/decrypted/example.sdat",
    "/dev_hdd0/tmp/scratch.bin": {"host": "scratch.bin", "read_only": false}
  }
}
```

| Key | Meaning |
| --- | --- |
| `version` | Required, the integer `1`. Any other value is an error. |
| `game_dir` | Shorthand for `--game-dir`: mounts `/dev_bdvd` and `/app_home` (`<game_dir>/PS3_GAME/USRDIR`), both read-only. |
| `hdd_dir` | Shorthand for `--hdd-dir`: mounts `/dev_hdd0` and `/dev_hdd1` (`<hdd_dir>/dev_hdd0`, `<hdd_dir>/dev_hdd1`), both writable. |
| `sdata_cache` | Shorthand for `--sdata-cache`. |
| `mounts` | Object from a device root (one component, such as `/dev_usb000`) to `{"host": PATH, "read_only": BOOL, "block_size": N}`. `host` is required. `read_only` defaults to `true` for `/dev_bdvd` and `/app_home` and to `false` for every other device. `block_size` is the block size that `cellFsStat` reports (a power of two from 512 to 1048576); the default depends on the device. The host directory of a writable mount is created when missing. |
| `files` | Object from an exact guest file path to a host file, either `"PATH"` (read-only) or `{"host": PATH, "read_only": BOOL}`. An override is checked before the mounts and serves the host file for `cellFsOpen`, `cellFsStat` and `cellFsSdataOpen`; for an sdata open it replaces the cache copy, so it must be an already decrypted file. A read-only override must exist; a writable one is created on open when its directory exists. |

Rules:

- Relative host paths resolve against the directory of the configuration file, not the working
  directory.
- Guest paths must be absolute and normalised: no empty, `.` or `..` component and no trailing
  `/`. A mount point is a single component; a nested one (`/dev_hdd0/game`) is rejected as
  overlapping its device. A `files` key needs at least two components and may not lie below
  another `files` key. Override keys match the guest path case-sensitively, after the path the
  guest passes is normalised.
- Each device can be mounted once: `game_dir` together with a `/dev_bdvd` or `/app_home` mount,
  or `hdd_dir` together with a `/dev_hdd0` or `/dev_hdd1` mount, is a duplicate. Repeated keys
  in one JSON object are errors. A writable mount may not share or contain the host directory
  of another mount (two read-only mounts may share one). Devices that are not listed stay
  unmounted; an explicit `/dev_bdvd` mount does not imply `/app_home`.
- Unknown keys are ignored with a `vfs config: ... unknown key` warning.
- An invalid file, a missing read-only host directory or override file, or an overlap stops the
  run with exit code 4 and a one-line message that names the key.
- Overrides are not added to directory listings: `cellFsOpendir` lists what the host directory
  holds.

Precedence is command-line option, then configuration file, then default. `--game-dir` replaces
the `/dev_bdvd` and `/app_home` mounts of the file (not other mounts), `--hdd-dir` replaces
`/dev_hdd0` and `/dev_hdd1`, and `--sdata-cache` replaces `sdata_cache`. `files` have no
command-line form. Without `--elf`, the executable and `PARAM.SFO` are looked up through the VFS
(`/dev_bdvd/PS3_GAME/...`), so the `/dev_bdvd` mount of the file, and a `files` override of those
paths, applies.
