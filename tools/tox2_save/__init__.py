"""Tools for the save data of Tales of Xillia 2: decrypt, inspect, patch and install."""

from __future__ import annotations

EXIT_OK = 0
EXIT_IO = 1  # cannot read/write a file; key file missing or unparseable; refused output location
EXIT_USAGE = 2  # argparse errors and invalid option combinations; unknown --code name
EXIT_FORMAT = 3  # malformed PFD/SFO/SAVE/savepatch; unsupported or out-of-range code
EXIT_VERIFY = 4  # PFD file hash mismatch (wrong secure file id); decrypted SAVE has no valid header


class CliError(Exception):
    def __init__(self, code: int, message: str):
        super().__init__(message)
        self.code = code
