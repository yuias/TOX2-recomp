import contextlib
import copy
import hashlib
import io
import json
import os
import tempfile
import unittest
from pathlib import Path

import ps3tool
from ps3recomp import spu_images
from ps3recomp.info import EXIT_IO, EXIT_NOT_PPU_ELF, EXIT_PIN_MISMATCH
from ps3recomp.spu_images import RegistryError

from tests import elf_fixture as fx
from tests.elf_fixture import FixtureSegment, build_elf32_be

REPO_ROOT = Path(__file__).resolve().parents[2]

PT_NOTE = 4
CODE_WORDS = 16
CODE_LS = 0x3000
DATA_LS = 0x3100
DATA_FILESZ = 0x10
DATA_MEMSZ = 0x40

# Guest layout of the synthetic containers: one segment at CONTAINER_VA holding the images.
CONTAINER_VA = 0x10000
ELF_AT = 0x100
SECOND_ELF_AT = 0x400
RAW_AT = 0x800
PIC_AT = 0xA00
RAW_SIZE = 0x40
PIC_SIZE = 0x20


def spu_elf(entry: int = CODE_LS + 8, code_ls: int = CODE_LS, machine: int = 23,
            with_note: bool = True) -> bytes:
    segments = [
        FixtureSegment(1, code_ls, bytes(range(1, 1 + 4 * CODE_WORDS)), flags=5),
        FixtureSegment(1, DATA_LS, bytes(DATA_FILESZ), memsz=DATA_MEMSZ, flags=6),
    ]
    if with_note:
        segments.append(FixtureSegment(PT_NOTE, 0, bytes(0x34), flags=4))
    return build_elf32_be(entry=entry, segments=segments, machine=machine)


def container_elf(blobs: dict[int, bytes]) -> bytes:
    body = bytearray(0x1000)
    for offset, blob in blobs.items():
        body[offset:offset + len(blob)] = blob
    return fx.build_elf64_be(
        entry=0, segments=[FixtureSegment(1, CONTAINER_VA, bytes(body), flags=5)])


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def registry_doc(*images: dict) -> dict:
    return {"schema": 1, "images": list(images)}


def elf_entry(name: str = "t_elf", **extra) -> dict:
    entry = {"name": name, "container": "eboot", "ea": "0x%x" % (CONTAINER_VA + ELF_AT),
             "kind": "elf", "role": "spurs_task", "lowering": "stackfree", "gate": False,
             "extra_entries": []}
    entry.update(extra)
    return entry


def raw_entry(name: str = "t_raw", **extra) -> dict:
    entry = {"name": name, "container": "libsre", "ea": "0x%x" % (CONTAINER_VA + RAW_AT),
             "kind": "raw", "size": "0x%x" % RAW_SIZE, "ls_base": "0xa00", "entry": "0xa10",
             "role": "spurs_policy_module", "lowering": "stackfree", "gate": True,
             "extra_entries": []}
    entry.update(extra)
    return entry


def pic_entry(name: str = "t_pic", **extra) -> dict:
    entry = {"name": name, "container": "libsre", "ea": "0x%x" % (CONTAINER_VA + PIC_AT),
             "kind": "raw", "size": "0x%x" % PIC_SIZE, "ls_base": None, "role": "exit_handler",
             "lowering": "stackfree", "gate": False, "extra_entries": []}
    entry.update(extra)
    return entry


class SpuFixtureTest(unittest.TestCase):
    """Writes synthetic containers, a pin file and a registry into a temporary directory."""

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.tmp = Path(self._tmp.name)
        self.raw_blob = bytes((i * 7 + 3) & 0xFF for i in range(RAW_SIZE))
        self.pic_blob = bytes((i * 5 + 1) & 0xFF for i in range(PIC_SIZE))
        self.elf_blob = spu_elf()
        self.eboot = container_elf({ELF_AT: self.elf_blob,
                                    SECOND_ELF_AT: spu_elf(entry=0x4004, code_ls=0x4000)})
        self.libsre = container_elf({RAW_AT: self.raw_blob, PIC_AT: self.pic_blob,
                                     ELF_AT: self.elf_blob})
        self.eboot_path = self.write("EBOOT.elf", self.eboot)
        self.libsre_path = self.write("libsre.reloc.elf", self.libsre)
        self.config_path = self.write_json("target.json", {
            "eboot_elf": {"sha256": sha(self.eboot)},
            "libsre_reloc_elf": {"sha256": sha(self.libsre)},
        })

    def write(self, name: str, data: bytes) -> Path:
        path = self.tmp / name
        path.write_bytes(data)
        return path

    def write_json(self, name: str, doc) -> Path:
        return self.write(name, json.dumps(doc).encode("utf-8"))

    def containers(self):
        return spu_images.load_containers(self.eboot_path, self.libsre_path, self.config_path)

    def resolve(self, entry: dict):
        parsed = spu_images.parse_registry(registry_doc(entry)).images[0]
        return spu_images.resolve_image(parsed, self.containers())

    def run_tool(self, *argv):
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = ps3tool.main(["spu-extract", *argv])
        return code, out.getvalue(), err.getvalue()

    def extract_args(self, registry: Path, out_dir: Path, *extra) -> list[str]:
        return ["--eboot", str(self.eboot_path), "--libsre", str(self.libsre_path),
                "--registry", str(registry), "--target-config", str(self.config_path),
                "--out", str(out_dir), *extra]


class ElfImageTest(SpuFixtureTest):
    def test_segments_entry_and_hash(self):
        image = self.resolve(elf_entry())
        ea = CONTAINER_VA + ELF_AT
        self.assertEqual(image.data, self.elf_blob)
        self.assertEqual(image.sha256, sha(self.elf_blob))
        self.assertEqual(image.entry_ls, CODE_LS + 8)
        self.assertEqual((image.ls_start, image.ls_end), (CODE_LS, DATA_LS + DATA_MEMSZ))
        self.assertFalse(image.position_independent)
        code, data = image.segments
        # The guest address of a segment is the image address plus its file offset.
        self.assertEqual((code.ea - ea, code.ls, code.filesz, code.memsz, code.code),
                         (code.offset, CODE_LS, 4 * CODE_WORDS, 4 * CODE_WORDS, True))
        self.assertEqual((data.ls, data.filesz, data.memsz, data.code),
                         (DATA_LS, DATA_FILESZ, DATA_MEMSZ, False))
        self.assertEqual(image.segment_bytes(code), bytes(range(1, 1 + 4 * CODE_WORDS)))
        self.assertEqual(image.code_segments, (code,))

    def test_note_segment_belongs_to_the_image_but_not_to_ls(self):
        with_note = self.resolve(elf_entry())
        # The PT_NOTE is the last segment in the file, so the image extends past the LOAD data.
        load_end = max(s.offset + s.filesz for s in with_note.segments)
        self.assertGreater(with_note.size, load_end)
        self.assertEqual(len(with_note.segments), 2)

    def test_image_in_the_other_container(self):
        image = self.resolve(elf_entry("t_lib_elf", container="libsre"))
        self.assertEqual(image.data, self.elf_blob)

    def test_code_segment_at_another_ls_address(self):
        image = self.resolve(elf_entry("t_second", ea="0x%x" % (CONTAINER_VA + SECOND_ELF_AT)))
        (code,) = image.code_segments
        self.assertEqual((code.ls, image.entry_ls), (0x4000, 0x4004))
        # The LS range spans every loaded segment, here the data segment below the code.
        self.assertEqual((image.ls_start, image.ls_end), (DATA_LS, 0x4000 + 4 * CODE_WORDS))

    def test_not_an_spu_elf(self):
        eboot = container_elf({ELF_AT: spu_elf(machine=21)})
        self.eboot_path = self.write("EBOOT.elf", eboot)
        self.config_path = self.write_json("target.json", {
            "eboot_elf": {"sha256": sha(eboot)},
            "libsre_reloc_elf": {"sha256": sha(self.libsre)}})
        with self.assertRaisesRegex(spu_images.ImageError, "not an SPU ELF") as ctx:
            self.resolve(elf_entry())
        self.assertEqual(ctx.exception.code, EXIT_NOT_PPU_ELF)

    def test_no_elf_at_address(self):
        with self.assertRaisesRegex(spu_images.ImageError, "no ELF image"):
            self.resolve(elf_entry(ea="0x%x" % (CONTAINER_VA + 0x600)))

    def test_address_outside_the_container(self):
        with self.assertRaisesRegex(spu_images.ImageError, "not backed by file data"):
            self.resolve(elf_entry(ea="0x%x" % (CONTAINER_VA + 0x2000)))

    def test_entry_outside_code(self):
        eboot = container_elf({ELF_AT: spu_elf(entry=DATA_LS)})
        self.eboot_path = self.write("EBOOT.elf", eboot)
        self.config_path = self.write_json("target.json", {
            "eboot_elf": {"sha256": sha(eboot)},
            "libsre_reloc_elf": {"sha256": sha(self.libsre)}})
        with self.assertRaisesRegex(spu_images.ImageError, "entry 0x3100 is not in a code"):
            self.resolve(elf_entry())

    def test_extra_entry_must_be_code(self):
        good = elf_entry(extra_entries=[{"ls": "0x3010", "reason": "indirect target"}])
        self.assertEqual(self.resolve(good).entry.extra_entries[0].ls, 0x3010)
        bad = elf_entry(extra_entries=[{"ls": "0x3200", "reason": "not code"}])
        with self.assertRaisesRegex(RegistryError, "extra entry 0x3200 is not in a code"):
            self.resolve(bad)


class RawImageTest(SpuFixtureTest):
    def test_raw_blob(self):
        image = self.resolve(raw_entry())
        self.assertEqual(image.data, self.raw_blob)
        self.assertEqual(image.sha256, sha(self.raw_blob))
        self.assertEqual((image.ls_start, image.ls_end, image.entry_ls),
                         (0xA00, 0xA00 + RAW_SIZE, 0xA10))
        (seg,) = image.segments
        self.assertEqual((seg.ea, seg.ls, seg.filesz, seg.memsz, seg.code, seg.offset),
                         (CONTAINER_VA + RAW_AT, 0xA00, RAW_SIZE, RAW_SIZE, True, 0))

    def test_entry_defaults_to_ls_base(self):
        entry = raw_entry()
        del entry["entry"]
        self.assertEqual(self.resolve(entry).entry_ls, 0xA00)

    def test_position_independent_blob(self):
        image = self.resolve(pic_entry())
        self.assertEqual(image.data, self.pic_blob)
        self.assertTrue(image.position_independent)
        self.assertEqual((image.ls_start, image.ls_end, image.entry_ls), (None, None, None))
        self.assertEqual(len(image.code_segments), 1)

    def test_blob_outside_the_container(self):
        with self.assertRaisesRegex(spu_images.ImageError, "outside a loaded segment"):
            self.resolve(raw_entry(ea="0x%x" % (CONTAINER_VA + 0x1000 - 0x10)))


class RegistryTest(unittest.TestCase):
    def parse(self, *images):
        return spu_images.parse_registry(registry_doc(*images))

    def assertRejected(self, pattern, *images, doc=None):
        with self.assertRaisesRegex(RegistryError, pattern) as ctx:
            spu_images.parse_registry(doc if doc is not None else registry_doc(*images))
        self.assertEqual(ctx.exception.code, 2)

    def test_valid_entries(self):
        registry = self.parse(elf_entry(), raw_entry(), pic_entry())
        self.assertEqual([e.name for e in registry.images], ["t_elf", "t_raw", "t_pic"])
        self.assertEqual(registry.find("t_raw").ls_base, 0xA00)
        self.assertIsNone(registry.find("t_pic").ls_base)
        self.assertIsNone(registry.find("nope"))

    def test_schema(self):
        self.assertRejected("schema", doc={"schema": 2, "images": [elf_entry()]})
        self.assertRejected("schema", doc=[])
        self.assertRejected("non-empty", doc={"schema": 1, "images": []})
        self.assertRejected("unknown top-level", doc={"schema": 1, "images": [elf_entry()],
                                                      "extra": 1})

    def test_name(self):
        self.assertRejected("name must match", elf_entry("Upper"))
        self.assertRejected("name must match", elf_entry("1abc"))
        self.assertRejected("name must match", elf_entry(""))
        self.assertRejected("duplicate image name t_elf", elf_entry(), elf_entry())

    def test_fields(self):
        self.assertRejected("container must be", elf_entry(container="game"))
        self.assertRejected("kind must be", elf_entry(kind="blob"))
        self.assertRejected("unknown role", elf_entry(role="boss"))
        self.assertRejected("gate must be", elf_entry(gate="yes"))
        self.assertRejected("ea must be a hex string", elf_entry(ea=1234))
        self.assertRejected("not word aligned", elf_entry(ea="0x1002"))
        self.assertRejected("unknown or misplaced keys: color", elf_entry(color="red"))
        entry = elf_entry()
        del entry["gate"]
        self.assertRejected("missing keys: gate", entry)

    def test_elf_entries_take_their_layout_from_the_elf(self):
        self.assertRejected("misplaced keys: size", elf_entry(size="0x10"))
        self.assertRejected("misplaced keys: ls_base", elf_entry(ls_base="0x3000"))

    def test_raw_entries(self):
        entry = raw_entry()
        del entry["size"]
        self.assertRejected("missing keys: size", entry)
        entry = raw_entry()
        del entry["ls_base"]
        self.assertRejected("missing keys: ls_base", entry)
        self.assertRejected("non-zero multiple of 4", raw_entry(size="0x0"))
        self.assertRejected("non-zero multiple of 4", raw_entry(size="0x42"))
        self.assertRejected("does not fit", raw_entry(ls_base="0x3ffc0", entry="0x3ffc0",
                                                      size="0x80"))
        self.assertRejected("entry 0xa40 is outside the blob", raw_entry(entry="0xa40"))
        self.assertRejected("not a word aligned LS address", raw_entry(ls_base="0xa02"))
        self.assertRejected("not a word aligned LS address", raw_entry(ls_base="0x40000"))

    def test_position_independent_blob_has_no_entries(self):
        self.assertRejected("position independent", pic_entry(entry="0x0"))
        self.assertRejected("position independent", pic_entry(
            extra_entries=[{"ls": "0x10", "reason": "x"}]))

    def test_extra_entries(self):
        self.assertRejected("extra_entries must be a list", elf_entry(extra_entries={}))
        self.assertRejected("non-empty \"reason\"", elf_entry(extra_entries=[{"ls": "0x10"}]))
        self.assertRejected("non-empty \"reason\"", elf_entry(
            extra_entries=[{"ls": "0x10", "reason": " "}]))
        self.assertRejected("word aligned", elf_entry(
            extra_entries=[{"ls": "0x11", "reason": "x"}]))
        self.assertRejected("duplicate extra_entries ls 0x10", elf_entry(
            extra_entries=[{"ls": "0x10", "reason": "x"}, {"ls": "0x10", "reason": "y"}]))

    def test_lowering_not_implemented(self):
        self.assertRejected("lowering not implemented", elf_entry(lowering="hostcall"))
        self.assertRejected("lowering not implemented", elf_entry(
            lowering_overrides={"0x3010": "hostcall"}))
        self.assertRejected("lowering_overrides must be an object", elf_entry(
            lowering_overrides=[]))
        ok = self.parse(elf_entry(lowering_overrides={"0x3010": "stackfree"}))
        self.assertEqual(ok.images[0].lowering_overrides, ((0x3010, "stackfree"),))

    def test_registry_entry_hash_is_stable_and_changes_with_content(self):
        first = self.parse(elf_entry()).images[0]
        reordered = dict(reversed(list(elf_entry().items())))
        self.assertEqual(spu_images.registry_entry_sha256(first),
                         spu_images.registry_entry_sha256(self.parse(reordered).images[0]))
        other = self.parse(elf_entry(role="spurs_kernel")).images[0]
        self.assertNotEqual(spu_images.registry_entry_sha256(first),
                            spu_images.registry_entry_sha256(other))

    def test_missing_and_malformed_files(self):
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaises(spu_images.CliError) as ctx:
                spu_images.load_registry(Path(tmp) / "none.json")
            self.assertEqual(ctx.exception.code, EXIT_IO)
            bad = Path(tmp) / "bad.json"
            bad.write_text("{", encoding="utf-8")
            with self.assertRaisesRegex(RegistryError, "invalid SPU registry"):
                spu_images.load_registry(bad)


class SelectionTest(unittest.TestCase):
    def setUp(self):
        self.registry = spu_images.parse_registry(
            registry_doc(elf_entry("a_elf"), raw_entry("b_raw"), pic_entry("c_pic")))

    def names(self, names=None, pic=False):
        entries = spu_images.select_entries(self.registry, names,
                                            include_position_independent=pic)
        return [e.name for e in entries]

    def test_default_set(self):
        self.assertEqual(self.names(), ["a_elf", "b_raw"])
        self.assertEqual(self.names(pic=True), ["a_elf", "b_raw", "c_pic"])

    def test_named_images_keep_the_requested_order_without_duplicates(self):
        self.assertEqual(self.names("b_raw, a_elf,b_raw"), ["b_raw", "a_elf"])

    def test_errors(self):
        with self.assertRaisesRegex(RegistryError, "unknown image zzz"):
            self.names("a_elf,zzz")
        with self.assertRaisesRegex(RegistryError, "position independent"):
            self.names("c_pic")
        with self.assertRaisesRegex(RegistryError, "names no image"):
            self.names(" , ")
        self.assertEqual(self.names("c_pic", pic=True), ["c_pic"])


class ContainerPinTest(SpuFixtureTest):
    def test_matching_pins(self):
        containers = self.containers()
        self.assertEqual(containers["eboot"].sha256, sha(self.eboot))
        self.assertEqual(containers["libsre"].sha256, sha(self.libsre))

    def test_wrong_hash(self):
        self.write("EBOOT.elf", self.eboot + b"\0")
        with self.assertRaisesRegex(spu_images.CliError, "does not match the pinned eboot_elf") \
                as ctx:
            self.containers()
        self.assertEqual(ctx.exception.code, EXIT_PIN_MISMATCH)

    def test_missing_pin(self):
        self.config_path = self.write_json("target.json",
                                           {"eboot_elf": {"sha256": sha(self.eboot)}})
        with self.assertRaisesRegex(spu_images.CliError, "no libsre_reloc_elf sha256 is pinned") \
                as ctx:
            self.containers()
        self.assertEqual(ctx.exception.code, EXIT_PIN_MISMATCH)

    def test_unreadable_container(self):
        os.remove(self.libsre_path)
        with self.assertRaises(spu_images.CliError) as ctx:
            self.containers()
        self.assertEqual(ctx.exception.code, EXIT_IO)

    def test_not_a_ppu_elf(self):
        self.write("EBOOT.elf", b"not an elf")
        self.config_path = self.write_json("target.json", {
            "eboot_elf": {"sha256": sha(b"not an elf")},
            "libsre_reloc_elf": {"sha256": sha(self.libsre)}})
        with self.assertRaises(spu_images.CliError) as ctx:
            self.containers()
        self.assertEqual(ctx.exception.code, EXIT_NOT_PPU_ELF)


class ExtractCommandTest(SpuFixtureTest):
    def setUp(self):
        super().setUp()
        self.registry_path = self.write_json("spu_images.json", registry_doc(
            elf_entry(), raw_entry(), pic_entry()))
        self.out_dir = self.tmp / "out" / "images"

    def run_extract(self, *extra):
        return self.run_tool(*self.extract_args(self.registry_path, self.out_dir, *extra))

    def test_writes_files_manifest_and_lines(self):
        code, out, err = self.run_extract()
        self.assertEqual((code, err), (0, ""))
        ea = CONTAINER_VA
        self.assertEqual(out.splitlines(), [
            "t_elf elf ea 0x%08x ls 0x3000-0x3140 entry 0x3008 code segments 1" % (ea + ELF_AT),
            "t_raw raw ea 0x%08x ls 0xa00-0xa40 entry 0xa10 code segments 1" % (ea + RAW_AT),
            "t_pic raw ea 0x%08x ls - entry - code segments 1" % (ea + PIC_AT),
        ])
        self.assertEqual((self.out_dir / "t_elf.elf").read_bytes(), self.elf_blob)
        self.assertEqual((self.out_dir / "t_raw.bin").read_bytes(), self.raw_blob)
        self.assertEqual((self.out_dir / "t_pic.bin").read_bytes(), self.pic_blob)

        manifest = json.loads((self.out_dir / "manifest.json").read_text(encoding="utf-8"))
        self.assertEqual(manifest["schema"], 1)
        elf, raw, pic = manifest["images"]
        self.assertEqual((elf["file"], elf["kind"], elf["ls_base"], elf["ls_end"], elf["entry"]),
                         ("t_elf.elf", "elf", "0x3000", "0x3140", "0x3008"))
        self.assertEqual(elf["sha256"], sha(self.elf_blob))
        self.assertEqual(elf["size"], len(self.elf_blob))
        self.assertEqual([(s["ls"], s["size"], s["memsz"], s["code"]) for s in elf["segments"]],
                         [("0x3000", 4 * CODE_WORDS, 4 * CODE_WORDS, True),
                          ("0x3100", DATA_FILESZ, DATA_MEMSZ, False)])
        self.assertEqual((raw["file"], raw["ls_base"], raw["entry"]),
                         ("t_raw.bin", "0xa00", "0xa10"))
        self.assertEqual(raw["segments"][0]["ea"], "0x%08x" % (ea + RAW_AT))
        self.assertIsNone(pic["ls_base"])
        self.assertIsNone(pic["entry"])

    def test_images_option_selects_and_limits_the_manifest(self):
        code, out, _ = self.run_extract("--images", "t_raw")
        self.assertEqual(code, 0)
        self.assertEqual(len(out.splitlines()), 1)
        manifest = json.loads((self.out_dir / "manifest.json").read_text(encoding="utf-8"))
        self.assertEqual([i["name"] for i in manifest["images"]], ["t_raw"])
        self.assertFalse((self.out_dir / "t_elf.elf").exists())

    def test_unknown_image(self):
        code, _, err = self.run_extract("--images", "nope")
        self.assertEqual(code, 2)
        self.assertIn("unknown image nope", err)

    def test_rerun_does_not_rewrite_unchanged_files(self):
        self.run_extract()
        files = [self.out_dir / n for n in ("t_elf.elf", "t_raw.bin", "manifest.json")]
        for path in files:
            os.utime(path, ns=(1_000_000_000, 1_000_000_000))
        code, _, _ = self.run_extract()
        self.assertEqual(code, 0)
        self.assertEqual([p.stat().st_mtime_ns for p in files], [1_000_000_000] * 3)

    def test_container_hash_mismatch_exits_4(self):
        self.write("libsre.reloc.elf", self.libsre + b"\0")
        code, out, err = self.run_extract()
        self.assertEqual(code, EXIT_PIN_MISMATCH)
        self.assertEqual(out, "")
        self.assertIn("does not match the pinned libsre_reloc_elf", err)
        self.assertFalse(self.out_dir.exists())

    def test_registry_error_exits_2(self):
        bad = copy.deepcopy(registry_doc(elf_entry(lowering="hostcall")))
        self.registry_path = self.write_json("spu_images.json", bad)
        code, _, err = self.run_extract()
        self.assertEqual(code, 2)
        self.assertIn("lowering not implemented", err)

    def test_missing_registry_exits_1(self):
        self.registry_path = self.tmp / "missing.json"
        code, _, err = self.run_extract()
        self.assertEqual(code, EXIT_IO)
        self.assertIn("cannot read SPU registry", err)

    def test_bad_image_exits_3(self):
        self.registry_path = self.write_json("spu_images.json", registry_doc(
            elf_entry(ea="0x%x" % (CONTAINER_VA + 0x600))))
        code, _, err = self.run_extract()
        self.assertEqual(code, EXIT_NOT_PPU_ELF)
        self.assertIn("no ELF image", err)


class CommittedRegistryTest(unittest.TestCase):
    """The registry in the repository, checked without any container."""

    GATE = {"libsre_kernel1", "libsre_system_service", "libsre_taskset_pm", "eboot_edgelzma"}

    @classmethod
    def setUpClass(cls):
        cls.registry = spu_images.load_registry(REPO_ROOT / "config" / "spu_images.json")

    def test_sixteen_images(self):
        names = [e.name for e in self.registry.images]
        self.assertEqual(len(names), 16)
        self.assertEqual(sum(n.startswith("eboot_") for n in names), 8)
        self.assertEqual(sum(n.startswith("libsre_") for n in names), 8)

    def test_gate_images(self):
        self.assertEqual({e.name for e in self.registry.images if e.gate}, self.GATE)

    def test_containers_follow_the_name_prefix(self):
        for e in self.registry.images:
            self.assertEqual(e.container, e.name.split("_")[0])

    def test_only_the_kernel_has_extra_entries(self):
        extra = {e.name: [(x.ls) for x in e.extra_entries]
                 for e in self.registry.images if e.extra_entries}
        self.assertEqual(extra, {"libsre_kernel1": [0x808]})

    def test_raw_blobs(self):
        raw = {e.name: (e.size, e.ls_base, e.entry)
               for e in self.registry.images if e.kind == "raw"}
        self.assertEqual(raw["libsre_system_service"], (0x2200, 0xA00, 0xA00))
        self.assertEqual(raw["libsre_taskset_pm"], (0x1E40, 0xA00, 0xA00))
        self.assertEqual(raw["libsre_taskset2_exit"], (0x670, None, None))
        self.assertEqual(raw["libsre_exit_code_plugin"], (0x710, None, None))
        self.assertEqual(raw["libsre_jobchain_pm1"], (0x3E30, 0xA00, 0xA00))
        self.assertEqual(raw["libsre_jobchain_pm2"], (0x3E30, 0xA00, 0xA00))

    def test_default_selection_skips_position_independent_blobs(self):
        known = spu_images.select_entries(self.registry, None, include_position_independent=False)
        self.assertEqual(len(known), 14)
        everything = spu_images.select_entries(self.registry, None,
                                               include_position_independent=True)
        self.assertEqual(len(everything), 16)

    def test_no_byte_dumps(self):
        text = (REPO_ROOT / "config" / "spu_images.json").read_text(encoding="utf-8")
        self.assertNotIn("\r", text)
        self.assertNotIn("\0", text)
        # Hashes belong in function databases, not in the registry.
        self.assertNotRegex(text, r"[0-9a-f]{64}")


if __name__ == "__main__":
    unittest.main()
