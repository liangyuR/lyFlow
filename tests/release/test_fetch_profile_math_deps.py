import importlib.util
import io
import json
from pathlib import Path
import tarfile
import tempfile
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("fetch_profile_math_deps", ROOT / "scripts/fetch-profile-math-deps.py")
FETCH = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(FETCH)


class ProfileMathDependencySafety(unittest.TestCase):
    def setUp(self):
        (ROOT / "build").mkdir(exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(prefix="profile-math-fixture-", dir=ROOT / "build")
        self.base = Path(self.temporary.name)
        self.output = self.base / "prepared"
        self.asset = json.loads((ROOT / "packaging/profile-math-deps.json").read_text())
        self.asset["keyHeaders"] = ["Eigen/Core", "Eigen/Version"]
        self.asset["licenseFiles"] = ["COPYING.MPL2"]
        self.entries = [("eigen-5.0.1/Eigen/Core", b"header payload\n"),
                        ("eigen-5.0.1/Eigen/Version", b"#define EIGEN_MAJOR_VERSION 5\n#define EIGEN_MINOR_VERSION 0\n#define EIGEN_PATCH_VERSION 1\n"),
                        ("eigen-5.0.1/COPYING.MPL2", b"license notice"),
                        ("eigen-5.0.1/cmake/Eigen3Config.cmake.in", b"@PACKAGE_INIT@"),
                        ("eigen-5.0.1/cmake/Eigen3ConfigVersion.cmake.in", b"@CVF_VERSION@")]
        self.archive_dir = self.base / "input"
        self.archive_dir.mkdir()
        self.configuration = self.base / "manifest.json"

    def tearDown(self):
        self.temporary.cleanup()

    def archive(self, entries, path=None):
        path = path or self.archive_dir / self.asset["archive"]
        with tarfile.open(path, "w:gz") as output:
            for name, payload in entries:
                entry = tarfile.TarInfo(name) if isinstance(name, str) else name
                if entry.isfile():
                    entry.size = len(payload)
                output.addfile(entry, io.BytesIO(payload) if entry.isfile() else None)
        return path

    def manifest(self, entries=None):
        path = self.archive(self.entries if entries is None else entries)
        self.asset.update(bytes=path.stat().st_size, sha512=FETCH.digest(path))
        self.configuration.write_text(json.dumps(self.asset))
        return path

    @staticmethod
    def install(source, install, work, asset, cmake):
        for name in asset["keyHeaders"]:
            header = install / "include/eigen3" / name
            header.parent.mkdir(parents=True, exist_ok=True)
            header.write_bytes((source / name).read_bytes())
        config = install / "share/eigen3/cmake"
        config.mkdir(parents=True)
        for name in asset["licenseFiles"]:
            (config.parent / name).write_bytes((source / name).read_bytes())
        for name in ["Eigen3Config.cmake", "Eigen3ConfigVersion.cmake", "Eigen3Targets.cmake"]:
            (config / name).write_text("Eigen3::Eigen fixture")

    def prepare(self, **options):
        return FETCH.prepare(self.configuration, self.output, self.archive_dir, offline=True, **options)

    def test_pin_matches_release_and_requires_fixed_official_archive(self):
        self.manifest()
        self.assertEqual(FETCH.read_manifest(self.configuration)["version"], "5.0.1")
        for key, value in [("version", "5.0.2"), ("url", "https://example.com/eigen.tar.gz"),
                           ("archive", "../eigen.tar.gz"), ("sha512", "0" * 127), ("bytes", True),
                           ("keyHeaders", ["Eigen/../../escape"]), ("licenseFiles", [])]:
            with self.subTest(key=key):
                changed = {**self.asset, key: value}
                self.configuration.write_text(json.dumps(changed))
                with self.assertRaises(ValueError):
                    FETCH.read_manifest(self.configuration)

    def test_offline_prepare_verify_and_dirty_source_install_archive_rejection(self):
        archive = self.manifest()
        with mock.patch.object(FETCH, "install_headers", side_effect=self.install), mock.patch.object(FETCH.urllib.request, "urlopen", side_effect=AssertionError("network")):
            eigen_dir = self.prepare()
            self.assertEqual(eigen_dir, self.output / "install/share/eigen3/cmake")
            before = (self.output / FETCH.STAMP).read_bytes()
            self.prepare(verify_only=True)
            self.assertEqual(before, (self.output / FETCH.STAMP).read_bytes())
            for relative in ["source/Eigen/Core", "install/include/eigen3/Eigen/Core"]:
                path = self.output / relative
                original = path.read_bytes()
                path.write_bytes(b"dirty")
                for verify in [False, True]:
                    with self.assertRaisesRegex(ValueError, "changed"):
                        self.prepare(verify_only=verify)
                    self.assertEqual(path.read_bytes(), b"dirty")
                path.write_bytes(original)
            cache = self.output.with_name(self.output.name + "-archives") / self.asset["archive"]
            cache.write_bytes(b"corrupt")
            with self.assertRaisesRegex(ValueError, "SHA512"):
                self.prepare(verify_only=True)
            self.assertEqual(archive.stat().st_size, self.asset["bytes"])

    def test_unmanaged_and_missing_outputs_never_download_or_overwrite(self):
        self.manifest()
        with mock.patch.object(FETCH.urllib.request, "urlopen", side_effect=AssertionError("network")):
            with self.assertRaises(FileNotFoundError):
                self.prepare(verify_only=True)
            self.output.mkdir()
            (self.output / "user.txt").write_text("keep")
            with self.assertRaisesRegex(ValueError, "Unmanaged"):
                self.prepare()
            self.assertEqual((self.output / "user.txt").read_text(), "keep")
            self.assertFalse((self.output / FETCH.STAMP).exists())
        with self.assertRaises(FileNotFoundError):
            FETCH.acquire(self.asset, self.base, None, True)

    def test_extraction_rejects_traversal_absolute_windows_and_links_before_writes(self):
        unsafe = ["../escape", "/absolute", "root/../escape", "C:/escape", "root\\escape", "root/NUL.txt", "root/name. ", "root/a:b"]
        for kind in [tarfile.SYMTYPE, tarfile.LNKTYPE, tarfile.FIFOTYPE, tarfile.CHRTYPE]:
            entry = tarfile.TarInfo("root/link")
            entry.type, entry.linkname = kind, "../../escape"
            unsafe.append(entry)
        for name in unsafe:
            with self.subTest(name=name):
                archive = self.archive([(name, b"bad")])
                with self.assertRaises(ValueError):
                    FETCH.safe_extract(archive, self.base / "extract")
                self.assertFalse((self.base / "extract").exists())
                self.assertFalse((self.base / "escape").exists())

    def test_extraction_rejects_collisions_budgets_corruption_and_preserves_valid_payload(self):
        cases = [[("root/a", b"a"), ("root/A", b"b")], [("one/a", b"a"), ("two/b", b"b")],
                 [("root/Dir/a", b"a"), ("root/dir/b", b"b")], [("root/a", b"a"), ("root/a/b", b"b")],
                 [("root/a/b", b"a"), ("root/a", b"b")], [("root/a", b"a"), ("root/a", b"a")]]
        for entries in cases:
            with self.subTest(entries=entries):
                archive = self.archive(entries)
                with self.assertRaises(ValueError):
                    FETCH.safe_extract(archive, self.base / "extract")
                self.assertFalse((self.base / "extract").exists())
        archive = self.archive([("root/a", b"123456"), ("root/b", b"b")])
        for limit in ["MAX_FILES", "MAX_EXPANDED", "MAX_MEMBER"]:
            with mock.patch.object(FETCH, limit, 1):
                with self.assertRaises(ValueError):
                    FETCH.safe_extract(archive, self.base / "extract")
                self.assertFalse((self.base / "extract").exists())
        archive.write_bytes(b"corrupt gzip")
        with self.assertRaises(tarfile.TarError):
            FETCH.safe_extract(archive, self.base / "extract")
        archive = self.archive([("root/nested/header", b"unmodified header")])
        FETCH.safe_extract(archive, self.base / "extract")
        self.assertEqual((self.base / "extract/nested/header").read_bytes(), b"unmodified header")

    def test_failed_source_validation_or_cmake_install_leaves_no_prepared_tree(self):
        for entries in [self.entries[:-1], [(name, data.replace(b"VERSION 5", b"VERSION 4")) for name, data in self.entries]]:
            self.manifest(entries)
            with self.assertRaises(ValueError):
                self.prepare()
            self.assertFalse(self.output.exists())
            cached = self.output.with_name(self.output.name + "-archives") / self.asset["archive"]
            cached.unlink()
        self.manifest()
        with mock.patch.object(FETCH, "install_headers", side_effect=ValueError("CMake failed")):
            with self.assertRaisesRegex(ValueError, "CMake failed"):
                self.prepare()
        self.assertFalse(self.output.exists())
        self.assertFalse(list(self.base.glob("prepared-prepare-*")))
        with mock.patch.object(FETCH, "install_headers", return_value=None):
            with self.assertRaisesRegex(ValueError, "Installed Eigen header"):
                self.prepare()
        self.assertFalse(self.output.exists())

    def test_download_checks_hash_budget_and_removes_partial_archive(self):
        archive = self.manifest()
        original = archive.read_bytes()
        for payload in [original + b"oversized", b"x" * len(original)]:
            with self.subTest(size=len(payload)):
                with self.assertRaises(ValueError):
                    FETCH.cache_archive(io.BytesIO(payload), self.base / "cached.tar.gz", self.asset)
                self.assertFalse((self.base / "cached.tar.gz").exists())
                self.assertFalse(list(self.base.glob("*.part")))


if __name__ == "__main__":
    unittest.main()
