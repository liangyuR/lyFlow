import hashlib
import importlib.util
import io
import json
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock


REPOSITORY = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("p4_assets", REPOSITORY / "scripts/fetch-p4-model-assets.py")
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class Response(io.BytesIO):
    def geturl(self):
        return "https://example.org/fixed.bin"


class P4Assets(unittest.TestCase):
    def setUp(self):
        (REPOSITORY / "build").mkdir(exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(prefix="p4-asset-test-", dir=REPOSITORY / "build")
        self.root = Path(self.temporary.name)
        self.configuration = self.root / "assets.json"
        self.output = self.root / "prepared"
        self.cache = self.root / "cache"
        self.cache.mkdir()
        self.payload = b"fixed public test asset\n"
        self.asset = {"name": "model.bin", "bytes": len(self.payload), "sha256": hashlib.sha256(self.payload).hexdigest(), "urls": ["https://example.org/fixed.bin"]}
        self.document = {"schemaVersion": 1, "assets": [self.asset]}
        self.write_manifest()

    def tearDown(self):
        self.temporary.cleanup()

    def write_manifest(self):
        self.configuration.write_text(json.dumps(self.document), encoding="utf-8")

    def fetch(self, offline=True, verify=False):
        MODULE.fetch(self.configuration, self.output, [self.cache], offline, verify)

    def test_offline_local_and_cached_roundtrip_always_rechecks_bytes(self):
        self.asset["reuseName"] = "original.bin"
        (self.cache / "original.bin").write_bytes(self.payload)
        local = self.root / "repository/examples/scene.png"
        local.parent.mkdir(parents=True)
        local.write_bytes(self.payload)
        second = dict(self.asset, name="scene.png", local="examples/scene.png")
        del second["urls"]
        self.document["assets"].append(second)
        self.write_manifest()
        with mock.patch.object(MODULE, "REPOSITORY", self.root / "repository"), mock.patch.object(MODULE.urllib.request, "build_opener") as network:
            self.fetch()
            self.fetch(verify=True)
            self.assertFalse(network.return_value.open.called)
        self.assertEqual((self.output / "model.bin").read_bytes(), self.payload)
        self.assertEqual((self.output / "scene.png").read_bytes(), self.payload)
        self.assertFalse(list(self.output.glob("*.part")))
        (self.output / "model.bin").write_bytes(b"x" * len(self.payload))
        with self.assertRaisesRegex(ValueError, "Existing asset"):
            self.fetch(verify=True)
        self.assertEqual((self.output / "model.bin").read_bytes(), b"x" * len(self.payload))

    def test_bad_cache_is_not_repaired_or_bypassed_with_network(self):
        cached = self.cache / "model.bin"
        for value in (b"short", b"x" * len(self.payload)):
            with self.subTest(value=value):
                cached.write_bytes(value)
                with mock.patch.object(MODULE.urllib.request, "build_opener") as network:
                    with self.assertRaisesRegex(ValueError, "Cached/repository"):
                        self.fetch(offline=False)
                    self.assertFalse(network.return_value.open.called)
                self.assertEqual(cached.read_bytes(), value)
                self.assertFalse((self.output / "model.bin").exists())

    def test_prepared_output_reuses_canonical_name_even_with_legacy_reuse_name(self):
        self.asset["reuseName"] = "legacy.bin"
        self.write_manifest()
        (self.cache / "legacy.bin").write_bytes(self.payload)
        self.fetch()
        next_output = self.root / "next-prepared"
        MODULE.fetch(self.configuration, next_output, [self.output], offline=True)
        self.assertEqual((next_output / "model.bin").read_bytes(), self.payload)
        MODULE.fetch(self.configuration, next_output, offline=True, verify_only=True)
        (self.output / "legacy.bin").write_bytes(b"bad legacy candidate")
        rejected_output = self.root / "rejected"
        with self.assertRaisesRegex(ValueError, "Cached/repository"):
            MODULE.fetch(self.configuration, rejected_output, [self.output], offline=True)
        self.assertFalse((rejected_output / "model.bin").exists())
        self.assertFalse(list(rejected_output.glob("*.part")))

    def test_verify_only_and_offline_missing_never_download(self):
        with mock.patch.object(MODULE.urllib.request, "build_opener") as network:
            with self.assertRaises(FileNotFoundError):
                self.fetch(verify=True)
            self.assertFalse(self.output.exists())
            self.output.mkdir()
            with self.assertRaises(FileNotFoundError):
                self.fetch(verify=True)
            with self.assertRaises(FileNotFoundError):
                self.fetch()
            self.assertFalse(network.return_value.open.called)
        self.assertEqual(list(self.output.iterdir()), [])

    def test_portable_names_paths_and_https_sources_reject_escapes(self):
        for value in ("../bad", "C:drive", "\\bad", "NUL.txt", "com1", "name.", "name ", "a/b", "bad\x00", "x:y", ""):
            with self.subTest(name=repr(value)), self.assertRaises(ValueError):
                MODULE.filename(value)
        for value in ("../outside", "C:/outside", "C:\\outside", "/outside", "a/../b", "a/./b", "a//b", "\\\\host\\file"):
            with self.subTest(local=value), self.assertRaises(ValueError):
                MODULE.local_path(value)
        for value in ("http://example.org/x", "https://u:p@example.org/x", "https://@example.org/x", "https://example.org/x#fragment", "https://example.org:80/x", "https://example.org/\nx", 4):
            with self.subTest(url=value), self.assertRaises(ValueError):
                MODULE.source_url(value)
        handler = MODULE.HttpsRedirects()
        with self.assertRaises(ValueError):
            handler.redirect_request(None, None, 302, "", {}, "http://example.org/downgrade")

    def test_manifest_budgets_types_and_case_collisions_fail_before_output(self):
        cases = [{"bytes": True}, {"bytes": 0}, {"bytes": MODULE.MAX_ASSET_BYTES + 1}, {"sha256": "a" * 63}, {"urls": "https://example.org/x"}, {"urls": []}, {"reuseName": "../bad"}, {"local": "../bad"}]
        for change in cases:
            with self.subTest(change=change):
                self.document["assets"] = [dict(self.asset, **change)]
                self.write_manifest()
                with self.assertRaises(ValueError):
                    self.fetch()
                self.assertFalse(self.output.exists())
        self.document["assets"] = [self.asset, dict(self.asset, name="MODEL.BIN")]
        self.write_manifest()
        with self.assertRaisesRegex(ValueError, "Duplicate"):
            self.fetch()
        self.document["assets"] = [dict(self.asset, name=f"m{i}", bytes=MODULE.MAX_ASSET_BYTES) for i in range(9)]
        self.write_manifest()
        with self.assertRaisesRegex(ValueError, "512MiB"):
            self.fetch()
        self.configuration.write_bytes(b" " * (256 * 1024 + 1))
        with self.assertRaisesRegex(ValueError, "256KiB"):
            self.fetch()

    def test_download_fixed_hash_fallback_and_oversize_cleanup(self):
        self.asset["urls"].append("https://example.org/mirror.bin")
        self.write_manifest()
        opener = mock.Mock()
        opener.open.side_effect = [Response(b"bad"), Response(self.payload)]
        with mock.patch.object(MODULE.urllib.request, "build_opener", return_value=opener):
            self.fetch(offline=False)
        self.assertEqual(opener.open.call_count, 2)
        self.assertEqual((self.output / "model.bin").read_bytes(), self.payload)
        (self.output / "model.bin").unlink()
        opener.open.side_effect = [Response(self.payload + b"too large"), OSError("unavailable")]
        with mock.patch.object(MODULE.urllib.request, "build_opener", return_value=opener):
            with self.assertRaisesRegex(ValueError, "All public sources"):
                self.fetch(offline=False)
        self.assertEqual(list(self.output.iterdir()), [])

    def test_publish_race_and_changed_copy_leave_no_partial_output(self):
        (self.cache / "model.bin").write_bytes(self.payload)
        def replace_copy(original, destination, maximum):
            destination.write(b"changed")
        with mock.patch.object(MODULE, "copy_bounded", side_effect=replace_copy):
            with self.assertRaisesRegex(ValueError, "changed during copy"):
                self.fetch()
        self.assertEqual(list(self.output.iterdir()), [])
        def competing_file(source, destination):
            destination.write_bytes(b"another writer")
            raise FileExistsError("target already exists")
        with mock.patch.object(MODULE.os, "link", side_effect=competing_file):
            with self.assertRaises(FileExistsError):
                self.fetch()
        self.assertEqual((self.output / "model.bin").read_bytes(), b"another writer")
        self.assertFalse(list(self.output.glob("*.part")))

    def test_link_reparse_and_ancestor_gate_without_windows_only_constants(self):
        fake = mock.Mock()
        fake.lstat.return_value = SimpleNamespace(st_file_attributes=0x400)
        fake.is_symlink.return_value = False
        with mock.patch.object(MODULE, "stat", SimpleNamespace()):
            self.assertTrue(MODULE.link_like(fake))
        fake.lstat.return_value = SimpleNamespace()
        with mock.patch.object(MODULE, "stat", SimpleNamespace()):
            self.assertFalse(MODULE.link_like(fake))
        original = MODULE.link_like
        def linked_ancestor(path):
            return path == self.root or original(path)
        with mock.patch.object(MODULE, "link_like", side_effect=linked_ancestor):
            with self.assertRaisesRegex(ValueError, "Link/reparse"):
                self.fetch()
        self.assertFalse(self.output.exists())

    def test_shipped_manifest_local_fixture_and_license_links(self):
        document = MODULE.manifest(REPOSITORY / "packaging/p4-model-assets.json")
        names = {asset["name"] for asset in document["assets"]}
        for asset in document["assets"]:
            if "licenseAsset" in asset:
                self.assertIn(asset["licenseAsset"], names)
            if "local" in asset:
                self.assertTrue(MODULE.matches(REPOSITORY / asset["local"], asset))
                truth = json.loads((REPOSITORY / asset["truth"]).read_text(encoding="utf-8"))
                self.assertEqual(truth["sha256"], asset["sha256"])
                self.assertEqual(truth["bytes"], asset["bytes"])


if __name__ == "__main__":
    unittest.main()
