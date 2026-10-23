# Save data tool

`tools/tox2_savetool.py` works on the save data of Tales of Xillia 2. It needs Python 3.10 or newer and
uses only the standard library. It does four things:

- `decrypt` turns a save directory from a real PS3 (US `BLUS31397`, with `PARAM.PFD` and an encrypted
  `SAVE`) into a plain directory, which is the form the runtime reads. Decryption goes one way only; the
  tool cannot encrypt, rebuild `PARAM.PFD` or export a save back to a PS3.
- `info` prints the section table and the known fields of a plain `SAVE`.
- `patch` applies Game Genie codes to a plain `SAVE`.
- `install` copies a plain save into the runtime's hdd tree as a slot of the Japanese build
  (`BLJS10188`).

```sh
python3 tools/tox2_savetool.py decrypt <save_dir> -o OUT_DIR [--keys DIR] [--secure-file-id HEX]
python3 tools/tox2_savetool.py info <SAVE> [--json]
python3 tools/tox2_savetool.py patch --patch FILE --list
python3 tools/tox2_savetool.py patch <SAVE> [--patch FILE --code NAME [--code NAME ...]] [--raw CODE ...] (-o OUT | --in-place)
python3 tools/tox2_savetool.py install <plain_dir> --hdd DIR [--slot N]
```

The tool never writes into a source directory. Every command builds its whole output in memory and
writes it only after all checks have passed, so an error writes nothing. Input files (`PARAM.PFD`,
`PARAM.SFO`, `SAVE`, savepatch files) are treated as untrusted: offsets and sizes are range-checked, and
a malformed file gives an error line, not a traceback.

## Exit codes

An error prints one `error: <message>` line on stderr. Notes go to stderr as `note: ...`; normal output
goes to stdout.

| Code | Meaning |
| --- | --- |
| 0 | Success (also `--help`) |
| 1 | A file or directory cannot be read or written; `key_vault.h` is missing or has no valid key; the output location is refused (non-empty directory, existing file, existing slot, no free slot) |
| 2 | Usage error: unknown option, invalid option combination, unknown `--code` name, `--slot` outside 1..999, a malformed `--secure-file-id`, an output directory that is the save directory or inside it |
| 3 | A malformed `PARAM.PFD`, `PARAM.SFO`, `SAVE` or savepatch file; an unsupported or out-of-range code; an encrypted `SAVE` given to `info`, `patch` or `install` |
| 4 | The file hash of an encrypted file does not match `PARAM.PFD` (wrong secure file id); the decrypted `SAVE` has no valid header (wrong key) |

## Keys

`decrypt` needs the PS3 system key `SC_ISO_SERIES_KEY_1`. The tool reads it from the `key_vault.h` that
the build downloads when it is configured with `-EbootBin` or `-Pup` (`build.sh --eboot-bin` or
`--pup`), in `generated/deps/rpcs3/<commit>/`; the commit is the one in `cmake/rpcs3_keys.cmake`. See
[Keys](building.md#keys). `--keys DIR` names another directory that holds `key_vault.h`. The key is not
stored in this repository, and no command, error message or log line prints its bytes. If `key_vault.h`
is missing, the error names the two build options and `docs/building.md`. The key is read only when the
save has at least one encrypted file.

The secure file id of the game (`2E7F0BC9F5383AB3DC4E8EA303F04D8A`) is built into the tool. It is a
per-title identifier that is published in the PFD tools' game lists, not a console key. It decides the
HMAC key of the per-file hash in `PARAM.PFD`. `--secure-file-id HEX` (32 hex digits) replaces it, for a
save of another title. If the id is wrong, `decrypt` stops with exit code 4 before it writes anything.
The tool checks only this per-file hash; it does not verify the signature or the other hash tables of
`PARAM.PFD`.

## decrypt

`decrypt <save_dir> -o OUT_DIR` does this:

1. `save_dir` must be a directory (exit 1). `OUT_DIR` must not be the save directory or inside it (2),
   and must be missing or empty (1, `refusing to write into non-empty <dir>`).
2. `save_dir` must have a `PARAM.PFD` (3). A directory without one is not an encrypted PS3 save; the
   saves the runtime writes are plain already.
3. Each file that `PARAM.PFD` lists is decrypted, except `PARAM.SFO`, which is stored in plain and is
   copied as it is. A file listed but missing is exit 3. The file hash is checked first (exit 4 on a
   mismatch). A decrypted `SAVE` must have a valid header, otherwise the key is wrong (exit 4).
4. Every other regular file of `save_dir` (for example `ICON0.PNG`) is copied as it is, except
   `PARAM.PFD`. Subdirectories are skipped with a note.
5. `OUT_DIR` is created (with its parents) and written.

Output, sorted by file name:

```
ICON0.PNG: copied
PARAM.SFO: copied
SAVE: decrypted (707840 bytes)
output: <OUT_DIR>
```

Decrypting the US save takes about 0.7 s, because the AES is pure Python.

## info

`info <SAVE>` reads a plain `SAVE` and prints one item per line:

```
size:       <len> (total size field <total_size>)
sections:   <count>
section <index> offset 0x<offset> size 0x<size> <name>
gald:       <gald>
play time:  <h>:<mm>:<ss> (<frames> frames)
character <k> <name>: hp <hp> tp <tp> exp <exp> ac <ac> sp <sp> str <s> int <i> vit <v> dex <d> agi <a> spi <p>
```

A section without a name prints `(unnamed)`; the game's `SAVE` has a few. `--json` prints the same data
as JSON (schema 1: `size`, `total_size`, `sections`, `gald`, `play_time_frames`, `characters`; offsets
are strings such as `"0x00008890"`). A `SAVE` that is still encrypted fails with exit 3 and a message
that points to `decrypt`.

### Known fields

A real `SAVE` is 0xACD00 bytes with 44 sections. All values are big-endian. A file shorter than the last
known field is exit 3 for `info`.

| Field | Offset | Type |
| --- | --- | --- |
| Header: magic `0x0064`, section count (u16), total size (u32); 16-byte section entries from 0x10 (name offset, data offset, size, 0) | 0x00 | |
| Play time, in frames at 60 fps | 0x2DC | u32 |
| Gald | 0x2E8 | u32 |
| Character block `k` | 0x8890 + k * 0xA000 | |
| HP | block + 0x20 | u32 |
| TP | block + 0x24 | u32 |
| EXP | block + 0x30 | u32 |
| AC | block + 0x3F | u8 |
| SP | block + 0x40 | u32 |
| Strength, intelligence, vitality, dexterity, agility, spirit | block + 0x46, 4 bytes apart | u16 each |

The characters, in block order, are Alvin, Jude, Milla, Elize, Rowen, Leia, Gaius, Muzet and Ludger
(`k` = 0 to 8). The order comes from the community codes that set all stats of each character. The
section names do not follow this order. The tool does not repair any checksum, because none is known for
this game.

## patch

`patch --patch FILE --list` prints the codes of a savepatch file, in file order, with a `[GROUP:<g>]`
line whenever the group changes and each code name on its own line, exactly as in the file.

`patch <SAVE> ...` applies codes to a copy of `SAVE`:

- `--patch FILE --code NAME` selects a code of a savepatch file by its exact name. `--code` may be
  repeated, and needs `--patch`.
- `--raw CODE` gives Game Genie lines directly, as pairs of 8 hex digits separated by white space (for
  example `--raw "200002E8 00989680"`). It may be repeated; one `--raw` can hold a two-line code.
- At least one `--code` or `--raw` is required. The selected codes are applied in order: the `--code`
  values first, then the `--raw` values; a later write wins.
- Exactly one of `-o OUT` and `--in-place` is required. `-o` never overwrites a file (exit 1) and may not
  name the input (exit 2, use `--in-place`). `--in-place` is the only way the tool modifies its input.
- `--list` cannot be combined with `SAVE`, `--code`, `--raw`, `-o` or `--in-place`.
- `SAVE` must be a plain `SAVE` with a valid header; an encrypted one is exit 3. All writes are checked
  against the file size before the first one is applied, so a code that writes past the end changes
  nothing.

Output:

```
code '<name>': <n> write(s)
raw <i>: <n> write(s)
output: <path>
```

### Supported codes

Only these Game Genie forms are supported. Anything else is exit 3 and names the line.

| Form | Meaning |
| --- | --- |
| `00AAAAAA 000000VV`, `10AAAAAA 0000VVVV`, `20AAAAAA VVVVVVVV` | Write 1, 2 or 4 bytes at offset `AAAAAA`. The first digit is the type (0, 1 or 2) and the second digit must be 0. The value must fit the width. |
| `4TAAAAAA VVVVVVVV` followed by `4NNNWWWW IIIIIIII` | Repeat write. `T` is 0, 1 or 2 for a width of 1, 2 or 4 bytes. `NNN` is the number of writes, including the first (not 0). `WWWW` is the address step and `IIIIIIII` the value increment. Write `i` stores `V + i * I` (wrapped to the width) at `AAAAAA + i * WWWW`. |

Pointer and search codes and all other types are not supported, and neither are Apollo script commands.

### Savepatch files

A savepatch file is Apollo's text format. It is read as UTF-8 (a byte order mark and CRLF line ends are
accepted). Lines, after stripping white space:

- A blank line, or a line that starts with `;`, is skipped.
- `:<name>` sets the target of the codes that follow. A code with a target other than `SAVE` is exit 3
  when it is selected.
- `[GROUP:<g>]` sets the group of the codes that follow. It is not selectable.
- `[<name>]` starts a code. The name is everything between the brackets (it may contain `=`, `,`, `:`
  and `/`), and it must be unique in the file.
- Any other line belongs to the current code. Outside a code it is exit 3.

Body lines are checked only when their code is selected, so a file with unsupported codes can still be
used for the others.

The tool does not include or download savepatch files. Apollo's community patches are at
<https://github.com/bucanero/apollo-patches> (`BLJS10188.savepatch` and `BLUS31397.savepatch`, GPL-3.0).
The Japanese and US files have the same codes.

## install

`install <plain_dir> --hdd DIR` copies a plain save into `DIR`, the directory that `tox2_game --hdd-dir`
takes, as `DIR/dev_hdd0/home/00000001/savedata/BLJS10188_SAVE1_<NNN>`.

- `plain_dir` must be a directory (exit 1) without a `PARAM.PFD` (exit 3, run `decrypt` first), with a
  `PARAM.SFO` that parses and a `SAVE` with a valid header (exit 3).
- `--hdd DIR` must be an existing directory (exit 1). The save root below it is created only after every
  check has passed, so a failed run leaves `DIR` as it was.
- `--slot N` (1 to 999; otherwise exit 2) names the slot. An existing slot is exit 1. Without `--slot`
  the tool takes the lowest number from 001 to 999 that is not used. The game numbers its slots from 1.
  If all are used, the exit code is 1.
- Every regular file of `plain_dir` is copied, except `PARAM.SFO`. Subdirectories and files with names
  outside `[A-Za-z0-9_][A-Za-z0-9_.-]{0,63}` are skipped with a note.
- `PARAM.SFO` is written again, in the form the runtime writes. It keeps `TITLE`, `SUB_TITLE`, `DETAIL`,
  `SAVEDATA_LIST_PARAM`, `ATTRIBUTE` and `PARENTAL_LEVEL` of the source, and sets `SAVEDATA_DIRECTORY` to
  the slot name, `CATEGORY` to `SD` and `ACCOUNT_ID` to zeros. It has one `*<file>` entry per file: the
  source's value if it has one, otherwise 0 for `ICON0.PNG`, `ICON1.PAM`, `PIC1.PNG` and `SND0.AT3`
  (not secure) and 1 for the others, such as `SAVE`. The binary entries of a PS3 `PARAM.SFO` (`PARAMS`,
  `PARAMS2`) are dropped, because the runtime would read them as text and write them back damaged.

Output:

```
slot:   BLJS10188_SAVE1_001
files:  ICON0.PNG, PARAM.SFO, SAVE
output: <hdd>/dev_hdd0/home/00000001/savedata/BLJS10188_SAVE1_001
```

## Using a US save in the Japanese build

```sh
python3 tools/tox2_savetool.py decrypt <us_save_dir> -o <tmp>/us_save
python3 tools/tox2_savetool.py info <tmp>/us_save/SAVE
python3 tools/tox2_savetool.py patch <tmp>/us_save/SAVE --raw "200002E8 00989680" --in-place   # optional
python3 tools/tox2_savetool.py install <tmp>/us_save --hdd <hdd-dir>
tox2_game --hdd-dir <hdd-dir> ...
```

The `info` and `patch` steps are optional. After the run starts, the log should show the save in the
list and a `tox2: savedata: file op ... 'SAVE'` line with no `cannot read` error after it. The save data
handlers are described in [Runtime](runtime.md). Whether the game accepts a US `SAVE` depends on the data
inside it; the tool does not change the contents of `SAVE`.

## Tests

The tests are `tools/tests/test_save_*.py` and run with the other Python unit tests; see
[Python unit tests](tests.md#python-unit-tests). `test_save_real.py` runs against real saves and is
skipped unless `TOX2_US_SAVE_DIR` and `TOX2_JP_SAVE_DIR` are set. The `install` tests write only into
temporary directories.
