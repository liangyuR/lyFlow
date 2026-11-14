import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import tempfile
import unittest
import zipfile


REPOSITORY = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("barcode_preparation", REPOSITORY / "scripts/fetch-barcode-deps.py")
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class BarcodeSources(unittest.TestCase):
    def setUp(self):
        (REPOSITORY / "build").mkdir(exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(prefix="barcode-source-test-", dir=REPOSITORY / "build")
        self.root = Path(self.temporary.name)
        self.repository = self.root / "repository"
        (self.repository / "scripts").mkdir(parents=True)
        shutil.copyfile(REPOSITORY / "scripts/fetch-matching-deps.py", self.repository / "scripts/fetch-matching-deps.py")
        (self.repository / "packaging/patches").mkdir(parents=True)
        self.patch = self.repository / "packaging/patches/fix.patch"
        self.patch.write_text("--- a/sample.txt\n+++ b/sample.txt\n@@ -1 +1 @@\n-old\n+new\n", encoding="utf-8", newline="\n")
        self.archives = self.root / "archives"
        self.archives.mkdir()
        self.archive = self.archives / "zxing-cpp-3.1.1.zip"
        self.write_archive({"source/sample.txt": "old\n", "source/LICENSE": "test notice\n"})
        self.configuration = self.repository / "packaging/barcode-deps.json"
        self.manifest = {"schemaVersion": 1, "patchRevision": 1, "assets": [{
            "name": "zxing", "repository": "zxing-cpp/zxing-cpp", "tag": "v3.1.1",
            "revision": "a" * 40, "url": "https://github.com/zxing-cpp/zxing-cpp/releases/download/v3.1.1/zxing-cpp-3.1.1.zip",
            "archive": self.archive.name, "bytes": self.archive.stat().st_size,
            "sha256": hashlib.sha256(self.archive.read_bytes()).hexdigest(), "patch": "fix.patch", "licenseFiles": ["LICENSE"]
        }]}
        self.write_manifest()
        self.output = self.root / "prepared"

    def tearDown(self):
        self.temporary.cleanup()

    def write_archive(self, files):
        with zipfile.ZipFile(self.archive, "w") as archive:
            for name, data in files.items():
                archive.writestr(name, data)

    def write_manifest(self):
        self.configuration.write_text(json.dumps(self.manifest), encoding="utf-8")

    def prepare(self, verify=False):
        MODULE.prepare(self.repository, self.output, self.archives, True, verify)

    def test_prepare_and_verify_apply_fixed_patch_without_download(self):
        self.prepare()
        self.assertEqual((self.output / "zxing/sample.txt").read_text(), "new\n")
        self.prepare(True)
        self.prepare()

    def test_reject_modified_source_and_missing_summary(self):
        self.prepare()
        (self.output / "prepared.json").unlink()
        with self.assertRaises(ValueError):
            self.prepare(True)
        (self.output / "zxing/sample.txt").write_text("tampered\n")
        with self.assertRaises(ValueError):
            self.prepare()

    def test_reject_changed_patch_protocol_and_source_revision(self):
        self.prepare()
        original = self.patch.read_text()
        self.patch.write_text(original + "\n")
        with self.assertRaises(ValueError):
            self.prepare(True)
        self.patch.write_text(original, newline="\n")
        helper = self.repository / "scripts/fetch-matching-deps.py"
        helper.write_text(helper.read_text() + "\n")
        with self.assertRaises(ValueError):
            self.prepare(True)
        self.manifest["assets"][0]["revision"] = "b" * 40
        self.write_manifest()
        with self.assertRaises(ValueError):
            self.prepare(True)

    def test_reject_wrong_archive_bytes_and_unprepared_verify(self):
        with self.assertRaises(FileNotFoundError):
            self.prepare(True)
        with self.archive.open("ab") as stream:
            stream.write(b"wrong")
        with self.assertRaises(ValueError):
            self.prepare()
        self.assertFalse((self.output / "zxing").exists())

    def test_reject_unsafe_archive_paths_and_nonofficial_url(self):
        self.write_archive({"source/../escape.txt": "bad", "source/LICENSE": "notice"})
        self.manifest["assets"][0]["bytes"] = self.archive.stat().st_size
        self.manifest["assets"][0]["sha256"] = hashlib.sha256(self.archive.read_bytes()).hexdigest()
        self.write_manifest()
        with self.assertRaises(ValueError):
            self.prepare()
        self.assertFalse((self.root / "escape.txt").exists())
        self.manifest["assets"][0]["url"] = "https://example.com/other.zip"
        self.write_manifest()
        with self.assertRaises(ValueError):
            self.prepare()


if __name__ == "__main__":
    unittest.main()
