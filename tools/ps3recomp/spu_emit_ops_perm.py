"""Group `perm`: shuffle, quadword shifts and rotates, generate controls, select masks and gathers.

The lane semantics live in `runtime/include/tox2/spu_ops.h`. The quadword shifts and rotates take the
count word the instruction names (the preferred word of RB, or the sign-extended I7) and mask it
themselves, so a register form and its immediate form share one helper. SPU ISA 1.2, chapters
"Memory-Load/Store" (generate controls), "Constant-formation" (`fsmbi`), "Integer and logical"
(`fsm*`, `gb*`, `orx`, `shufb`) and "Shift and rotate" (quadword forms).
"""

from __future__ import annotations

from .spu_emit_core import SpuOp, handler, u32

# Register form: RT = helper(RA, preferred word of RB). The "...i" form of each takes I7 instead.
_QUADWORD = {
    "shlqbi": "tox2SpuShlqbi", "shlqby": "tox2SpuShlqby", "shlqbybi": "tox2SpuShlqbybi",
    "rotqbi": "tox2SpuRotqbi", "rotqby": "tox2SpuRotqby", "rotqbybi": "tox2SpuRotqbybi",
    "rotqmbi": "tox2SpuRotqmbi", "rotqmby": "tox2SpuRotqmby", "rotqmbybi": "tox2SpuRotqmbybi",
}

# The mnemonics that have an immediate form (the "from bit shift count" forms do not).
_QUADWORD_IMM = {name + "i": fn for name, fn in _QUADWORD.items() if not name.endswith("bybi")}

# Generate controls: element size in bytes. The x forms add the preferred words of RA and RB, the d forms
# add the sign-extended I7 to the preferred word of RA.
_CONTROL_SIZE = {"b": 1, "h": 2, "w": 4, "d": 8}

# Select-mask forms take the preferred word of RA.
_MASK = {"fsmb": "tox2SpuFsmb", "fsmh": "tox2SpuFsmh", "fsm": "tox2SpuFsm"}

_GATHER = {"gbb": "tox2SpuGbb", "gbh": "tox2SpuGbh", "gb": "tox2SpuGb", "orx": "tox2SpuOrx"}


def _set(op: SpuOp, expr: str, reg: str = "RT") -> list[str]:
    return ["%s = %s;" % (op.gpr(op.f(reg)), expr)]


def _make_quadword(name: str, fn: str):
    def emit(op: SpuOp) -> list[str]:
        return _set(op, "%s(%s, %s)" % (fn, op.gpr(op.f("RA")), op.pref(op.f("RB"))))
    handler(name)(emit)


def _make_quadword_imm(name: str, fn: str):
    def emit(op: SpuOp) -> list[str]:
        return _set(op, "%s(%s, %s)" % (fn, op.gpr(op.f("RA")), u32(op.f("I7"))))
    handler(name)(emit)


def _make_control(name: str, size: int):
    if name.endswith("x"):
        def emit(op: SpuOp) -> list[str]:
            return _set(op, "tox2SpuGenControl(%s + %s, %d)" % (op.pref(op.f("RA")), op.pref(op.f("RB")), size))
    else:
        def emit(op: SpuOp) -> list[str]:
            return _set(op, "tox2SpuGenControl(%s + %s, %d)" % (op.pref(op.f("RA")), u32(op.f("I7")), size))
    handler(name)(emit)


def _make_mask(name: str, fn: str):
    def emit(op: SpuOp) -> list[str]:
        return _set(op, "%s(%s)" % (fn, op.pref(op.f("RA"))))
    handler(name)(emit)


def _make_gather(name: str, fn: str):
    def emit(op: SpuOp) -> list[str]:
        return _set(op, "%s(%s)" % (fn, op.gpr(op.f("RA"))))
    handler(name)(emit)


for _name, _fn in _QUADWORD.items():
    _make_quadword(_name, _fn)
for _name, _fn in _QUADWORD_IMM.items():
    _make_quadword_imm(_name, _fn)
for _letter, _size in _CONTROL_SIZE.items():
    _make_control("c%sx" % _letter, _size)
    _make_control("c%sd" % _letter, _size)
for _name, _fn in _MASK.items():
    _make_mask(_name, _fn)
for _name, _fn in _GATHER.items():
    _make_gather(_name, _fn)


@handler("fsmbi")
def emit_fsmbi(op: SpuOp) -> list[str]:
    return _set(op, "tox2SpuFsmb(%s)" % u32(op.f("U16")))


@handler("shufb")
def emit_shufb(op: SpuOp) -> list[str]:
    return _set(op, "tox2SpuShufb(%s, %s, %s)" % (op.gpr(op.f("RA")), op.gpr(op.f("RB")), op.gpr(op.f("RC"))),
                "RT4")
