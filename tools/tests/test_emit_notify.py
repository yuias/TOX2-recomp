"""Tests for the notifying stores of the `emit` command (`--notify-stores`)."""

import contextlib
import hashlib
import io
import json
import re
import struct
import unittest
from unittest import mock

from ps3recomp import cell, emit, emit_core
from ps3recomp.elf import ElfFile
from ps3recomp.emit_core import EmitEnv, EmitStats, emit_function
from ps3recomp.emit_db import DbFunction
from ps3recomp.emit_notify import (NOTIFY_MNEMONICS, check_emitted, image_name_of,
                                   load_notify_stores, report_section, store_addresses)
from ps3recomp.info import CliError
from ps3recomp.ppu_flow import CodeImage

from tests import ppc_asm as asm
from tests.elf_fixture import PS3_TEXT_VA, build_ps3_fixture
from tests.test_emit_db import TempDirTestCase, db_doc, hexs

BASE = 0x10000
STB = asm.encode("stb", rS=3, D=1, rA=4)
STH = asm.encode("sth", rS=3, D=2, rA=4)
STW = asm.encode("stw", rS=3, D=4, rA=4)
STD = asm.encode("std", rS=3, DS=8, rA=4)
STWU = asm.encode("stwu", rS=3, D=8, rA=4)
STWU_RA0 = asm.encode("stwu", rS=3, D=8, rA=0)
STWBRX = asm.encode("stwbrx", rS=3, rA=4, rB=5)
STMW = asm.encode("stmw", rS=30, D=0, rA=4)
STFD = asm.encode("stfd", frS=1, D=0, rA=4)
LWZ = asm.lwz(3, 0, 4)

# Function A holds every word below; function B is one word behind it.
WORDS_A = [STB, STH, STW, STD, STWU, STWBRX, STMW, STFD, LWZ, asm.blr()]
A = BASE
B = BASE + 4 * len(WORDS_A)
ADDR = {name: A + 4 * i for i, name in
        enumerate(("stb", "sth", "stw", "std", "stwu", "stwbrx", "stmw", "stfd", "lwz", "blr"))}
WORDS_B = [STW, asm.blr()]
FUNCTIONS = [DbFunction(A, None, ((A, B),)), DbFunction(B, None, ((B, B + 8),))]
IMAGE = CodeImage.from_words(BASE, WORDS_A + WORDS_B)


def emit_words(words, notify=(), start=BASE):
    image = CodeImage.from_words(start, words)
    env = EmitEnv({}, {}, {}, False, frozenset(notify))
    stats = EmitStats()
    addrs = [start + 4 * i for i in range(len(words))]
    text = emit_function(image, start, addrs, env, stats, name="fn_%08x" % start)
    return text, stats


class LoaderTest(TempDirTestCase):
    def write(self, doc, name="notify.json"):
        path = self.tmp / name
        path.write_text(doc if isinstance(doc, str) else json.dumps(doc), encoding="utf-8")
        return path

    def entry(self, **overrides):
        item = {"image": "main", "function": hexs(A), "stores": [hexs(ADDR["stb"])],
                "reason": "wakes the waiter"}
        item.update(overrides)
        return item

    def load(self, doc, *, image_name="main", explicit=True):
        return load_notify_stores(self.write(doc), explicit=explicit, image_name=image_name,
                                  image=IMAGE, functions=FUNCTIONS)

    def assertRejected(self, doc, text):
        with self.assertRaises(CliError) as caught:
            self.load(doc)
        self.assertEqual(caught.exception.code, 3)
        self.assertIn(text, str(caught.exception))

    def test_valid_entries_are_sorted_by_function_and_stores_by_address(self):
        entries = self.load({"notify_stores": [
            self.entry(function=hexs(B), stores=[hexs(B)], reason="b"),
            self.entry(stores=[hexs(ADDR["stw"]), hexs(ADDR["stb"])], reason="a")]})
        self.assertEqual([(e.function, e.stores, e.reason) for e in entries],
                         [(A, (ADDR["stb"], ADDR["stw"]), "a"), (B, (B,), "b")])
        self.assertEqual(store_addresses(entries), {ADDR["stb"], ADDR["stw"], B})

    def test_every_supported_store_form_is_accepted(self):
        stores = [hexs(ADDR[name]) for name in ("stb", "sth", "stw", "std", "stwu", "stwbrx")]
        entries = self.load({"notify_stores": [self.entry(stores=stores)]})
        self.assertEqual(len(entries[0].stores), 6)

    def test_empty_list_is_valid(self):
        self.assertEqual(self.load({"notify_stores": []}), [])

    def test_not_json_and_wrong_shape(self):
        self.assertRejected("{", "not JSON")
        self.assertRejected([], '"notify_stores"')
        self.assertRejected({"notify": []}, '"notify_stores"')
        self.assertRejected({"notify_stores": {}}, '"notify_stores"')
        self.assertRejected({"notify_stores": [], "extra": 1}, '"notify_stores"')

    def test_wrong_keys(self):
        good = self.entry()
        missing = {k: v for k, v in good.items() if k != "reason"}
        self.assertRejected({"notify_stores": [missing]}, "expected the keys")
        self.assertRejected({"notify_stores": [dict(good, extra=1)]}, "expected the keys")
        self.assertRejected({"notify_stores": ["x"]}, "expected the keys")

    def test_wrong_values(self):
        cases = {
            "image": dict(image="Libsre"),
            "image type": dict(image=1),
            "function": dict(function="011d97e4"),
            "store": dict(stores=["0xzz"]),
            "stores type": dict(stores="0x10000"),
            "empty stores": dict(stores=[]),
        }
        for label, change in cases.items():
            with self.subTest(label), self.assertRaises(CliError) as caught:
                self.load({"notify_stores": [self.entry(**change)]})
            self.assertEqual(caught.exception.code, 3)

    def test_empty_reason(self):
        self.assertRejected({"notify_stores": [self.entry(reason="")]}, "reason")
        self.assertRejected({"notify_stores": [self.entry(reason="  ")]}, "reason")
        self.assertRejected({"notify_stores": [self.entry(reason=None)]}, "reason")

    def test_unknown_function(self):
        self.assertRejected({"notify_stores": [self.entry(function=hexs(A + 4))]},
                            "is not a function of the database")

    def test_address_not_owned_by_the_function(self):
        self.assertRejected({"notify_stores": [self.entry(stores=[hexs(B)])]},
                            "%s: store is not owned by function %s" % (hexs(B), hexs(A)))
        self.assertRejected({"notify_stores": [self.entry(stores=[hexs(0x20000)])]}, "not owned")

    def test_duplicate_address(self):
        store = hexs(ADDR["stb"])
        self.assertRejected({"notify_stores": [self.entry(stores=[store, store])]},
                            "%s: store listed twice" % store)

    def test_duplicate_address_across_entries_and_duplicate_function(self):
        # Functions own disjoint addresses here, so a repeated address implies a repeated function.
        self.assertRejected({"notify_stores": [self.entry(), self.entry()]}, "listed twice")
        self.assertRejected({"notify_stores": [self.entry(), self.entry(stores=[hexs(ADDR["sth"])])]},
                            "function listed twice")

    def test_unsupported_mnemonics(self):
        for name in ("stmw", "stfd", "lwz"):
            with self.subTest(name):
                self.assertRejected({"notify_stores": [self.entry(stores=[hexs(ADDR[name])])]},
                                    "%s: %s cannot be routed" % (hexs(ADDR[name]), name))

    def test_address_that_does_not_decode(self):
        image = CodeImage.from_words(BASE, [0x00000000, asm.blr()])
        fn = DbFunction(BASE, None, ((BASE, BASE + 8),))
        path = self.write({"notify_stores": [self.entry(function=hexs(BASE), stores=[hexs(BASE)])]})
        with self.assertRaises(CliError) as caught:
            load_notify_stores(path, explicit=True, image_name="main", image=image, functions=[fn])
        self.assertIn("does not decode", str(caught.exception))

    def test_image_filter_main_and_prefix(self):
        doc = {"notify_stores": [
            self.entry(),
            self.entry(image="libsre", function=hexs(B), stores=[hexs(B)], reason="lib")]}
        self.assertEqual([(e.image, e.function) for e in self.load(doc)], [("main", A)])
        self.assertEqual([(e.image, e.function) for e in self.load(doc, image_name="libsre")],
                         [("libsre", B)])
        self.assertEqual(self.load(doc, image_name="other"), [])
        # Functions and addresses of another image are not looked up in this one.
        foreign = {"notify_stores": [
            self.entry(image="libsre", function="0x011d97e4", stores=["0x011d9b20"])]}
        self.assertEqual(self.load(foreign), [])
        self.assertRejected_for(foreign, "libsre", "is not a function of the database")

    def assertRejected_for(self, doc, image_name, text):
        with self.assertRaises(CliError) as caught:
            self.load(doc, image_name=image_name)
        self.assertIn(text, str(caught.exception))

    def test_image_filter_still_checks_the_shape_of_other_images(self):
        self.assertRejected({"notify_stores": [self.entry(image="libsre", reason="")]}, "reason")

    def test_missing_file(self):
        missing = self.tmp / "missing.json"
        self.assertEqual(load_notify_stores(missing, explicit=False, image_name="main", image=IMAGE,
                                            functions=FUNCTIONS), [])
        with self.assertRaises(CliError) as caught:
            load_notify_stores(missing, explicit=True, image_name="main", image=IMAGE,
                               functions=FUNCTIONS)
        self.assertEqual(caught.exception.code, 1)
        self.assertIn("missing.json", str(caught.exception))

    def test_image_name_of(self):
        self.assertEqual((image_name_of(""), image_name_of("libsre")), ("main", "libsre"))

    def test_report_section(self):
        entries = self.load({"notify_stores": [self.entry(stores=[hexs(ADDR["stw"]),
                                                                 hexs(ADDR["stb"])])]})
        self.assertEqual(report_section(entries), [{
            "image": "main", "function": hexs(A),
            "stores": [hexs(ADDR["stb"]), hexs(ADDR["stw"])], "reason": "wakes the waiter"}])

    def test_check_emitted_reports_a_listed_store_that_was_not_emitted(self):
        entries = self.load({"notify_stores": [self.entry(stores=[hexs(ADDR["stb"]),
                                                                 hexs(ADDR["stw"])])]})
        check_emitted(entries, {ADDR["stb"], ADDR["stw"]})
        with self.assertRaises(CliError) as caught:
            check_emitted(entries, {ADDR["stb"]})
        self.assertEqual(caught.exception.code, 3)
        self.assertIn(hexs(ADDR["stw"]), str(caught.exception))
        self.assertIn("stale", str(caught.exception))


class MnemonicTableTest(unittest.TestCase):
    def test_matches_the_integer_stores_of_the_memory_group(self):
        emit_core.load_handlers()
        from ps3recomp import emit_ops_mem
        table = emit_ops_mem._TABLE
        expected = {name for name, access in table.items()
                    if access.store and access.kind in ("int", "reversed")}
        self.assertEqual(set(NOTIFY_MNEMONICS), expected)
        self.assertEqual(len(NOTIFY_MNEMONICS), 19)


class EmissionTest(unittest.TestCase):
    def substituted(self, text):
        return re.sub(r"tox2Write(8|16|32|64)\(", r"tox2NotifyWrite\1(", text)

    def test_a_listed_store_keeps_its_arguments(self):
        for word, width in ((STB, 8), (STH, 16), (STW, 32), (STD, 64)):
            with self.subTest(width=width):
                plain, _ = emit_words([word, asm.blr()])
                routed, stats = emit_words([word, asm.blr()], notify=[BASE])
                self.assertIn("tox2Write%d(" % width, plain)
                self.assertNotIn("tox2NotifyWrite", plain)
                self.assertEqual(routed, self.substituted(plain))
                self.assertIn("tox2NotifyWrite%d(base, " % width, routed)
                self.assertEqual(stats.notified, {BASE})

    def test_an_unlisted_store_in_the_same_function_stays_plain(self):
        plain, _ = emit_words([STB, STW, asm.blr()])
        routed, stats = emit_words([STB, STW, asm.blr()], notify=[BASE + 4])
        self.assertEqual(stats.notified, {BASE + 4})
        stores = [line.strip() for line in routed.splitlines() if "Write" in line]
        self.assertEqual(len(stores), 2)
        self.assertTrue(stores[0].startswith("tox2Write8("))
        self.assertTrue(stores[1].startswith("tox2NotifyWrite32("))
        self.assertEqual(routed.replace("tox2NotifyWrite32(", "tox2Write32("), plain)

    def test_update_form_routes_the_access_and_keeps_the_register_update(self):
        plain, _ = emit_words([STWU, asm.blr()])
        routed, stats = emit_words([STWU, asm.blr()], notify=[BASE])
        self.assertEqual(routed, self.substituted(plain))
        self.assertIn("tox2NotifyWrite32(base, (uint32_t)t0, ", routed)
        self.assertIn("ctx->gpr[4] = t0;", routed)
        self.assertEqual(stats.notified, {BASE})

    def test_byte_reversed_store_routes_with_the_swapped_value(self):
        plain, _ = emit_words([STWBRX, asm.blr()])
        routed, _ = emit_words([STWBRX, asm.blr()], notify=[BASE])
        self.assertEqual(routed, self.substituted(plain))
        self.assertIn("tox2NotifyWrite32(base, ", routed)
        self.assertIn("__builtin_bswap32(", routed)

    def test_an_empty_set_changes_nothing(self):
        words = [STB, STH, STW, STD, STWU, STWBRX, STMW, STFD, LWZ, asm.blr()]
        text, stats = emit_words(words)
        self.assertNotIn("NotifyWrite", text)
        self.assertEqual(stats.notified, set())
        self.assertEqual(emit_words(words, notify=[])[0], text)

    def test_a_listed_store_that_is_unhandled_is_not_recorded(self):
        text, stats = emit_words([STWU_RA0, asm.blr()], notify=[BASE])
        self.assertIn("TOX2_UNHANDLED", text)
        self.assertEqual(stats.notified, set())

    def test_listed_address_outside_the_function_is_never_recorded(self):
        _, stats = emit_words([STB, asm.blr()], notify=[BASE + 0x100])
        self.assertEqual(stats.notified, set())


class EmitCliTest(TempDirTestCase):
    """End to end on the PS3 fixture, whose first function ends in `stdu r1,-0x70(r1)` at +4."""

    TEXT = PS3_TEXT_VA
    STDU = PS3_TEXT_VA + 4
    PATCHED = PS3_TEXT_VA + 8       # `li r11,0x8d` in the fixture; replaced by a stwu with rA = 0

    def setUp(self):
        super().setUp()
        data = build_ps3_fixture()
        word = struct.pack(">I", 0x3960008D)
        self.assertEqual(data.count(word), 1)
        self.original = data
        self.patched = data.replace(word, struct.pack(">I", STWU_RA0))
        self.elf_path = self.tmp / "fixture.elf"
        self.report = self.tmp / "report.json"
        self.default_notify = self.tmp / "default_notify.json"
        # The real default files must never leak into a unit test.
        for name, path in (("DEFAULT_NOTIFY_STORES", self.default_notify),
                           ("DEFAULT_OVERRIDES", self.tmp / "default_overrides.json")):
            patcher = mock.patch.object(emit, name, path)
            patcher.start()
            self.addCleanup(patcher.stop)
        self.use(self.original)

    def use(self, data):
        self.elf_path.write_bytes(data)
        sha = hashlib.sha256(data).hexdigest()
        text = cell.find_text_range(ElfFile.from_bytes(data))
        self.db = self.write_db(db_doc(sha=sha, text=text, functions=[
            (self.TEXT, [(self.TEXT, self.TEXT + 0xC)]),
            (self.TEXT + 0xC, [(self.TEXT + 0xC, self.TEXT + 0x14)]),
            (self.TEXT + 0x14, [(self.TEXT + 0x14, self.TEXT + 0x1C)])]))

    def run_cli(self, out_name, *extra):
        out, err = io.StringIO(), io.StringIO()
        argv = [str(self.elf_path), "--db", str(self.db), "--out", str(self.tmp / out_name),
                "--report", str(self.report), "--census-out", str(self.tmp / "census.json"),
                "--chunks", "3"]
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = emit.main(argv + list(extra))
        return code, out.getvalue(), err.getvalue()

    def snapshot(self, out_name):
        return {p.name: p.read_text(encoding="utf-8")
                for p in sorted((self.tmp / out_name).iterdir())}

    def write_notify(self, *items, name="notify.json"):
        path = self.tmp / name
        path.write_text(json.dumps({"notify_stores": list(items)}), encoding="utf-8")
        return path

    def item(self, **overrides):
        item = {"image": "main", "function": hexs(self.TEXT), "stores": [hexs(self.STDU)],
                "reason": "wakes the idle service"}
        item.update(overrides)
        return item

    def test_no_file_and_empty_file_give_identical_output(self):
        self.assertEqual(self.run_cli("plain")[0], 0)
        self.assertFalse(self.default_notify.exists())
        self.default_notify.write_text('{"notify_stores": []}', encoding="utf-8")
        self.assertEqual(self.run_cli("default_empty")[0], 0)
        self.assertEqual(self.run_cli("explicit_empty", "--notify-stores",
                                      str(self.write_notify()))[0], 0)
        plain = self.snapshot("plain")
        self.assertNotIn("NotifyWrite", "".join(plain.values()))
        self.assertEqual(self.snapshot("default_empty"), plain)
        self.assertEqual(self.snapshot("explicit_empty"), plain)

    def test_listed_store_changes_only_its_own_line(self):
        self.run_cli("plain")
        code, out, err = self.run_cli("routed", "--notify-stores", str(self.write_notify(self.item())))
        self.assertEqual((code, err), (0, ""))
        plain, routed = self.snapshot("plain"), self.snapshot("routed")
        self.assertEqual(sorted(routed), sorted(plain))
        changed = [name for name in plain if plain[name] != routed[name]]
        self.assertEqual(len(changed), 1)
        before, after = plain[changed[0]].splitlines(), routed[changed[0]].splitlines()
        self.assertEqual(len(before), len(after))
        diff = [(a, b) for a, b in zip(before, after) if a != b]
        self.assertEqual(len(diff), 1)
        self.assertEqual(diff[0][1], diff[0][0].replace("tox2Write64(", "tox2NotifyWrite64("))
        self.assertIn("tox2NotifyWrite64(", diff[0][1])
        self.assertEqual(out.splitlines()[-2], "notify stores: 1 in 1 functions")

    def test_default_file_is_used_when_present(self):
        self.default_notify.write_text(json.dumps({"notify_stores": [self.item()]}), encoding="utf-8")
        self.assertEqual(self.run_cli("routed")[0], 0)
        self.assertIn("tox2NotifyWrite64(", "".join(self.snapshot("routed").values()))

    def test_entries_of_another_image_are_ignored(self):
        self.run_cli("plain")
        path = self.write_notify(self.item(image="libsre", function="0x011d97e4",
                                           stores=["0x011d9b20"]))
        code, out, _ = self.run_cli("other", "--notify-stores", str(path))
        self.assertEqual(code, 0)
        self.assertEqual(self.snapshot("other"), self.snapshot("plain"))
        self.assertEqual(out.splitlines()[-2], "notify stores: 0 in 0 functions")

    def test_report_section(self):
        path = self.write_notify(self.item(), self.item(image="libsre"))
        self.assertEqual(self.run_cli("routed", "--notify-stores", str(path))[0], 0)
        report = json.loads(self.report.read_text(encoding="utf-8"))
        self.assertEqual(report["notify_stores"], [{
            "image": "main", "function": hexs(self.TEXT), "stores": [hexs(self.STDU)],
            "reason": "wakes the idle service"}])

    def test_no_notify_stores_reports_zero(self):
        code, out, _ = self.run_cli("plain")
        self.assertEqual(code, 0)
        self.assertEqual(out.splitlines()[-2], "notify stores: 0 in 0 functions")
        self.assertEqual(out.splitlines()[-1], "overrides: 0")
        self.assertEqual(json.loads(self.report.read_text(encoding="utf-8"))["notify_stores"], [])

    def test_stale_entry_exits_3_before_anything_is_written(self):
        self.use(self.patched)
        # The word at +8 decodes as a store the emitter does not write (stwu with rA = 0).
        item = self.item(stores=[hexs(self.PATCHED)])
        code, _, err = self.run_cli("stale", "--notify-stores", str(self.write_notify(item)))
        self.assertEqual(code, 3)
        self.assertIn("stale", err)
        self.assertIn(hexs(self.PATCHED), err)
        self.assertFalse((self.tmp / "stale").exists())

    def test_validation_failures_exit_3(self):
        cases = {
            "not a store": self.item(stores=[hexs(self.TEXT)]),
            "other function": self.item(stores=[hexs(self.TEXT + 0xC)]),
            "unknown function": self.item(function=hexs(self.TEXT + 4)),
            "empty reason": self.item(reason=""),
        }
        for label, item in cases.items():
            with self.subTest(label):
                code, _, err = self.run_cli("bad", "--notify-stores", str(self.write_notify(item)))
                self.assertEqual(code, 3, err)
                self.assertFalse((self.tmp / "bad").exists())

    def test_explicit_missing_file_is_an_io_error(self):
        code, _, err = self.run_cli("bad", "--notify-stores", str(self.tmp / "missing.json"))
        self.assertEqual(code, 1)
        self.assertIn("missing.json", err)


if __name__ == "__main__":
    unittest.main()
