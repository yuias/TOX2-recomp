"""Tests for tools/fetch-ffmpeg.sh without network access.

Each run points the script at a temporary repository root (TOX2_FETCH_FFMPEG_ROOT) that holds a copy
of the pin file with the hash of a test zip. curl is replaced by a stub that serves a local file or
fails, so a bug cannot reach the real download.
"""

from __future__ import annotations

import hashlib
import os
import shutil
import stat
import subprocess
import tempfile
import unittest
import zipfile
from pathlib import Path

TOOLS_DIR = Path(__file__).resolve().parents[1]
SCRIPT = TOOLS_DIR / "fetch-ffmpeg.sh"
SH = shutil.which("sh")

TAG = "autobuild-2026-01-02-03-04"
STEM = "ffmpeg-test-win64-lgpl-shared-9.0"
ASSET = f"{STEM}.zip"
GOOD_ENTRIES = {
    f"{STEM}/bin/avcodec-62.dll": b"dll\n",
    f"{STEM}/include/libavcodec/avcodec.h": b"// header\n",
    f"{STEM}/LICENSE.txt": b"licence\n",
}

# Stands in for curl: copies $CURL_FIXTURE to the --output file, or fails like a network error.
CURL_STUB = """#!/bin/sh
echo "$*" >> "$CURL_LOG"
if [ -z "${CURL_FIXTURE:-}" ]; then
    echo "curl: (6) Could not resolve host" >&2
    exit 6
fi
while [ $# -gt 0 ]; do
    case $1 in
        --output) out=$2; shift ;;
    esac
    shift
done
cp "$CURL_FIXTURE" "$out"
"""


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def make_zip(path: Path, entries: dict[str, bytes]) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(path, "w") as archive:
        for name, data in entries.items():
            archive.writestr(zipfile.ZipInfo(name), data)
    return path


@unittest.skipUnless(os.name == "posix" and SH, "needs a POSIX sh")
class FetchFfmpegShTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.world = Path(self._tmp.name).resolve()
        self.root = self.world / "repo"
        self.deps = self.root / "generated" / "deps" / "ffmpeg"
        self.final = self.deps / STEM
        self.zip = make_zip(self.world / "kept" / ASSET, GOOD_ENTRIES)
        self.write_pin(sha256(self.zip))
        self.stub_dir = self.world / "stub"
        self.stub_dir.mkdir()
        curl = self.stub_dir / "curl"
        curl.write_text(CURL_STUB, encoding="utf-8")
        curl.chmod(curl.stat().st_mode | stat.S_IXUSR)
        self.curl_log = self.world / "curl.log"

    def write_pin(self, digest: str, extra: str = ""):
        pin = self.root / "cmake" / "ffmpeg.cmake"
        pin.parent.mkdir(parents=True, exist_ok=True)
        pin.write_text(
            "# test pin\n"
            f'set(TOX2_FFMPEG_RELEASE_TAG "{TAG}")\n'
            f'set(TOX2_FFMPEG_ASSET "{ASSET}")\n'
            f'set(TOX2_FFMPEG_SHA256 "{digest}")\n' + extra,
            encoding="utf-8",
        )

    def run_script(self, *args: str, fixture: Path | None = None, cwd: Path | None = None):
        env = dict(os.environ)
        env["TOX2_FETCH_FFMPEG_ROOT"] = str(self.root)
        env["PATH"] = f"{self.stub_dir}{os.pathsep}{env.get('PATH', '')}"
        env["CURL_LOG"] = str(self.curl_log)
        env.pop("CURL_FIXTURE", None)
        if fixture is not None:
            env["CURL_FIXTURE"] = str(fixture)
        return subprocess.run(
            [SH, str(SCRIPT), *args],
            env=env,
            cwd=cwd or self.world,
            stdin=subprocess.DEVNULL,
            capture_output=True,
            text=True,
            timeout=120,
        )

    def assertOk(self, proc):
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)

    def assertFails(self, proc, text: str):
        self.assertEqual(proc.returncode, 1, proc.stdout + proc.stderr)
        self.assertIn(text, proc.stderr)
        self.assertNotIn("ffmpeg: ready", proc.stdout)

    def assertNothingExtracted(self):
        self.assertFalse(self.final.exists())
        leftovers = [p.name for p in self.deps.glob(".staging-*")] if self.deps.exists() else []
        self.assertEqual(leftovers, [])

    def mark_verified(self, text: str | None = None):
        self.final.mkdir(parents=True)
        (self.final / ".verified-sha256").write_text(
            text if text is not None else sha256(self.zip), encoding="utf-8"
        )

    # Usage

    def test_unknown_option_is_a_usage_error(self):
        proc = self.run_script("--nope")
        self.assertEqual(proc.returncode, 2, proc.stdout + proc.stderr)
        self.assertIn("fetch-ffmpeg: unexpected argument: --nope", proc.stderr)
        self.assertIn("usage: tools/fetch-ffmpeg.sh [--from <zip>] [--force]", proc.stderr)
        self.assertEqual(proc.stdout, "")
        self.assertFalse((self.root / "generated").exists())

    def test_from_without_a_path_is_a_usage_error(self):
        for args in (("--from",), ("--from", "  ")):
            with self.subTest(args=args):
                proc = self.run_script(*args)
                self.assertEqual(proc.returncode, 2, proc.stdout + proc.stderr)
                self.assertIn("--from needs a path", proc.stderr)
                self.assertIn("usage:", proc.stderr)

    # Hash check

    def test_wrong_hash_is_refused(self):
        wrong = make_zip(self.world / "wrong.zip", {"x": b"other\n"})
        proc = self.run_script("--from", str(wrong))
        self.assertFails(proc, "SHA-256 mismatch for --from file")
        self.assertIn(f"expected {sha256(self.zip)}, got {sha256(wrong)}", proc.stderr)
        self.assertFalse((self.root / "generated").exists())

    def test_wrong_hash_is_refused_even_when_up_to_date(self):
        self.mark_verified()
        wrong = make_zip(self.world / "wrong.zip", {"x": b"other\n"})
        self.assertFails(self.run_script("--from", str(wrong)), "SHA-256 mismatch")

    def test_missing_from_file(self):
        proc = self.run_script("--from", "nothing.zip")
        self.assertFails(proc, "--from file not found: nothing.zip")

    # Up to date

    def test_verified_marker_is_up_to_date(self):
        self.mark_verified(sha256(self.zip) + "\n")
        before = sorted(p.relative_to(self.root).as_posix() for p in self.root.rglob("*"))
        proc = self.run_script()
        self.assertOk(proc)
        self.assertEqual(proc.stdout.strip(), f"ffmpeg: up to date ({STEM})")
        self.assertEqual(
            sorted(p.relative_to(self.root).as_posix() for p in self.root.rglob("*")), before
        )
        self.assertFalse(self.curl_log.exists(), "curl was called")

    def test_marker_for_another_hash_is_not_up_to_date(self):
        self.mark_verified("0" * 64)
        proc = self.run_script()
        # It needs a download, which the stub refuses.
        self.assertFails(proc, "download failed")

    def test_uppercase_hash_in_the_pin_is_accepted(self):
        self.write_pin(sha256(self.zip).upper())
        self.assertOk(self.run_script("--from", str(self.zip)))
        self.assertEqual((self.final / ".verified-sha256").read_text(), sha256(self.zip))

    # Extraction

    def test_extracts_from_a_local_zip(self):
        kept = self.zip.read_bytes()
        proc = self.run_script("--from", str(self.zip))
        self.assertOk(proc)
        self.assertIn(f"ffmpeg: extracting {STEM}", proc.stdout)
        self.assertIn(f"ffmpeg: ready in generated/deps/ffmpeg/{STEM}", proc.stdout)
        for name, data in GOOD_ENTRIES.items():
            self.assertEqual((self.deps / name).read_bytes(), data)
        self.assertEqual((self.final / ".verified-sha256").read_bytes(), sha256(self.zip).encode())
        self.assertEqual(self.zip.read_bytes(), kept, "the --from zip was modified")
        self.assertNothingExtractedBeyondFinal()
        self.assertFalse(self.curl_log.exists(), "curl was called")
        again = self.run_script("--from", str(self.zip))
        self.assertOk(again)
        self.assertIn("up to date", again.stdout)

    def assertNothingExtractedBeyondFinal(self):
        self.assertEqual(sorted(p.name for p in self.deps.iterdir()), [STEM])

    def test_relative_from_path_uses_the_callers_directory(self):
        proc = self.run_script("--from", f"kept/{ASSET}", cwd=self.world)
        self.assertOk(proc)
        self.assertTrue((self.final / "bin" / "avcodec-62.dll").is_file())

    def test_force_extracts_again(self):
        self.assertOk(self.run_script("--from", str(self.zip)))
        stale = self.final / "stale.txt"
        stale.write_text("old", encoding="utf-8")
        self.assertOk(self.run_script("--from", str(self.zip)))
        self.assertTrue(stale.exists(), "an up-to-date run changed the tree")
        proc = self.run_script("--from", str(self.zip), "--force")
        self.assertOk(proc)
        self.assertIn("ffmpeg: ready", proc.stdout)
        self.assertFalse(stale.exists())

    def test_archive_without_bin_is_refused(self):
        bad = make_zip(self.world / "nobin.zip", {f"{STEM}/readme.txt": b"x\n"})
        self.write_pin(sha256(bad))
        proc = self.run_script("--from", str(bad))
        self.assertFails(proc, f"the archive does not contain '{STEM}/bin'")
        self.assertNothingExtracted()

    # Safety

    def test_unsafe_entries_are_refused(self):
        entries = (
            "../escape.txt",
            f"{STEM}/../../escape.txt",
            "/absolute.txt",
            "C:/drive.txt",
            f"{STEM}\\..\\escape.txt",
            "\\rooted.txt",
        )
        for name in entries:
            with self.subTest(entry=name):
                archive = make_zip(
                    self.world / "unsafe.zip", {**GOOD_ENTRIES, name: b"evil\n"}
                )
                self.write_pin(sha256(archive))
                proc = self.run_script("--from", str(archive))
                self.assertFails(proc, "unsafe entry in the archive")
                self.assertNothingExtracted()
                self.assertFalse((self.world / "escape.txt").exists())
                self.assertFalse((self.root / "generated" / "escape.txt").exists())

    def test_symbolic_link_entry_is_refused(self):
        archive = self.world / "link.zip"
        with zipfile.ZipFile(archive, "w") as zf:
            for name, data in GOOD_ENTRIES.items():
                zf.writestr(zipfile.ZipInfo(name), data)
            link = zipfile.ZipInfo(f"{STEM}/bin/link")
            link.create_system = 3
            link.external_attr = (stat.S_IFLNK | 0o777) << 16
            zf.writestr(link, "/etc")
        self.write_pin(sha256(archive))
        proc = self.run_script("--from", str(archive))
        self.assertFails(proc, "symbolic link")
        self.assertNothingExtracted()

    def test_entry_names_that_only_look_unsafe_are_accepted(self):
        entries = {**GOOD_ENTRIES, f"{STEM}/bin/a..b.txt": b"ok\n", f"{STEM}/doc/..hidden": b"ok\n"}
        archive = make_zip(self.world / "dots.zip", entries)
        self.write_pin(sha256(archive))
        self.assertOk(self.run_script("--from", str(archive)))
        self.assertTrue((self.final / "bin" / "a..b.txt").is_file())

    # Pin file

    def test_malformed_pin_is_reported(self):
        for name, replacement in (
            ("TOX2_FFMPEG_RELEASE_TAG", 'set(TOX2_FFMPEG_RELEASE_TAG "latest")'),
            ("TOX2_FFMPEG_ASSET", 'set(TOX2_FFMPEG_ASSET "ffmpeg.tar.gz")'),
            ("TOX2_FFMPEG_SHA256", 'set(TOX2_FFMPEG_SHA256 "abc")'),
        ):
            with self.subTest(name=name):
                self.write_pin(sha256(self.zip))
                pin = self.root / "cmake" / "ffmpeg.cmake"
                lines = [
                    replacement if line.startswith(f"set({name} ") else line
                    for line in pin.read_text(encoding="utf-8").splitlines()
                ]
                pin.write_text("\n".join(lines) + "\n", encoding="utf-8")
                proc = self.run_script("--from", str(self.zip))
                self.assertFails(proc, f"{name} is missing or malformed in cmake/ffmpeg.cmake")

    def test_missing_pin_file(self):
        (self.root / "cmake" / "ffmpeg.cmake").unlink()
        self.assertFails(self.run_script(), "pin file not found: cmake/ffmpeg.cmake")

    # Download (curl is a stub)

    def test_download_is_verified_and_extracted(self):
        proc = self.run_script(fixture=self.zip)
        self.assertOk(proc)
        url = f"https://github.com/BtbN/FFmpeg-Builds/releases/download/{TAG}/{ASSET}"
        self.assertIn(f"ffmpeg: downloading {url}", proc.stdout)
        self.assertIn(url, self.curl_log.read_text())
        self.assertTrue((self.final / "bin" / "avcodec-62.dll").is_file())
        self.assertEqual(sha256(self.deps / "download" / ASSET), sha256(self.zip))
        self.assertFalse((self.deps / "download" / f"{ASSET}.part").exists())

    def test_download_with_the_wrong_hash_is_discarded(self):
        wrong = make_zip(self.world / "wrong.zip", {"x": b"other\n"})
        proc = self.run_script(fixture=wrong)
        self.assertFails(proc, "SHA-256 mismatch for the download")
        self.assertEqual(list((self.deps / "download").iterdir()), [])
        self.assertNothingExtracted()

    def test_failed_download_suggests_from(self):
        proc = self.run_script()
        self.assertFails(proc, "download failed: curl: (6) Could not resolve host")
        self.assertIn("use --from with a copy of the zip", proc.stderr)
        self.assertEqual(list((self.deps / "download").iterdir()), [])

    def test_cached_download_is_reused_only_when_it_matches(self):
        cache = self.deps / "download"
        cache.mkdir(parents=True)
        shutil.copy(self.zip, cache / ASSET)
        self.assertOk(self.run_script())
        self.assertFalse(self.curl_log.exists(), "curl was called for a good cached download")
        shutil.rmtree(self.final)
        (cache / ASSET).write_bytes(b"corrupt")
        self.assertOk(self.run_script(fixture=self.zip))
        self.assertTrue(self.curl_log.exists())
        self.assertEqual(sha256(cache / ASSET), sha256(self.zip))


if __name__ == "__main__":
    unittest.main()
