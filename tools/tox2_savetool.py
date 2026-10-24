"""CLI entry point: decrypt, inspect, patch and install Tales of Xillia 2 save data."""

import sys

from tox2_save import cli

if __name__ == "__main__":
    sys.exit(cli.main(sys.argv[1:]))
