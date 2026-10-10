"""Tiny SPU encoder for tests: `encode` builds the word of any mnemonic in the decoder's table.

Field values are the decoder's: signed fields (`I7`, `I10`, `I16`) may be negative, `I10x16`, `REL16`,
`ABS16` and `RO` are byte values (multiples of 16 or 4), registers are plain numbers. Omitted fields
are 0. For branches and hints, `addr` and `target` (and `site` for hbra/hbrr/hbr) compute the
displacement fields. Mnemonics are the base names of the table (`bi`, not `bid`); the D/E/C/P bits are
fields.
"""

from ps3recomp import spu


def encode(mnemonic: str, *, addr: int | None = None, target: int | None = None, site: int | None = None,
           **fields: int) -> int:
    spec = spu.SPECS_BY_NAME.get(mnemonic)
    if spec is None:
        raise ValueError(f"unknown SPU mnemonic {mnemonic!r}")
    names = spec.field_names
    allowed = set(names) | set(spec.flag_fields)
    fields = dict(fields)
    if target is not None or site is not None:
        if addr is None:
            raise ValueError(f"{mnemonic}: target/site need addr")
        if target is not None:
            if "REL16" in names:
                fields["REL16"] = target - addr
            elif "ABS16" in names:
                # Absolute targets are sign-extended from 18 bits.
                fields["ABS16"] = target - (1 << 18) if target >= 1 << 17 else target
            else:
                raise ValueError(f"{mnemonic}: has no target field")
        if site is not None:
            key = "RO_IND" if "RO_IND" in names else "RO"
            if key not in names:
                raise ValueError(f"{mnemonic}: has no hint site field")
            fields[key] = site - addr
    word = spec.match
    used = 0
    for name, value in fields.items():
        if name not in allowed:
            raise ValueError(f"{mnemonic} has no field {name!r}")
        if isinstance(value, bool) or not isinstance(value, int):
            raise ValueError(f"{mnemonic}.{name}: value must be an int")
        field = spu.FIELDS[name]
        try:
            bits = field.insert(value)
        except ValueError as exc:
            raise ValueError(f"{mnemonic}.{name}: {exc}") from None
        # RT and RC share bits in forms that name only one of them, so overlap means a real conflict.
        if used & field.mask:
            raise ValueError(f"{mnemonic}.{name} overlaps another field")
        used |= field.mask
        word |= bits
    if word & spec.mask != spec.match:
        raise ValueError(f"{mnemonic}: fields overlap the opcode")
    return word
