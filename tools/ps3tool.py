"""CLI entry point: dispatches subcommands to `ps3recomp.<module>.main(argv)`."""

import importlib
import sys

# Subcommand name -> module under ps3recomp. Modules are imported lazily.
COMMANDS = {
    "info": "info",
    "probe": "probe",
    "disasm": "disasm",
    "ghidra-export": "ghidra",
    "ghidra-diff": "ghidra_diff",
    "ghidra-summary": "ghidra_summary",
    "recover": "recover",
    "emit": "emit",
    "nidgen": "nidgen",
    "spu-extract": "spu_images",
    "spu-disasm": "spu_disasm",
    "spu-recover": "spu_recover",
    "spu-emit": "spu_emit",
}


def _usage() -> str:
    return "usage: ps3tool.py {%s} [options] ..." % ",".join(COMMANDS)


def main(argv: list[str]) -> int:
    if argv and argv[0] in ("-h", "--help"):
        print(_usage())
        return 0
    if not argv or argv[0] not in COMMANDS:
        print(_usage(), file=sys.stderr)
        print("ps3tool.py: error: expected one of: %s" % ", ".join(COMMANDS),
              file=sys.stderr)
        return 2
    module = importlib.import_module("ps3recomp." + COMMANDS[argv[0]])
    return module.main(argv[1:])


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
