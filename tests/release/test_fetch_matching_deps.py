import importlib.util
from pathlib import Path
import stat
import tempfile
import unittest
from unittest import mock
import zipfile


SOURCE = Path(__file__).resolve().parents[2] / "scripts/fetch-matching-deps.py"
SPEC = importlib.util.spec_from_file_location("fetch_matching_deps", SOURCE)
FETCH = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(FETCH)


class MatchingDependencySafety(unittest.TestCase):
    def setUp(self):
        root = SOURCE.parents[1] / "build"
        root.mkdir(exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(prefix="matching-fetch-fixture-", dir=root)
        self.base = Path(self.temporary.name)

    def tearDown(self):
        self.temporary.cleanup()

    def archive(self, entries):
        archive = self.base / "fixture.zip"
        with zipfile.ZipFile(archive, "w") as output:
            for name, content in entries:
                if isinstance(name, str):
                    entry = zipfile.ZipInfo()
                    entry.filename = name
                    entry.orig_filename = name
                    name = entry
                output.writestr(name, content)
        return archive

    def test_extracts_one_source_root_without_modifying_payload(self):
        archive = self.archive([("package/source.cpp", "const int value = 1;\n"),
                                ("package/include/header.hpp", "#pragma once\n")])
        destination = self.base / "source"
        FETCH.safe_extract(archive, destination)
        self.assertEqual((destination / "source.cpp").read_text(), "const int value = 1;\n")
        self.assertEqual(len(FETCH.inventory(destination)), 2)

    def test_rejects_traversal_drive_backslash_and_reserved_paths(self):
        for name in ["../escape.txt", "/absolute.txt", "package/../escape.txt", "C:/escape.txt",
                     "package\\escape.txt", "package/NUL.txt", "package/file. ", "package/a:b",
                     "package/NUL .txt", "package/CON .txt", "package/COM¹.txt", "package/LPT².log",
                     "package/a?b", "package/a*b", "package/a<b", "package/a|b"]:
            with self.subTest(name=name):
                archive = self.archive([(name, "bad")])
                destination = self.base / "rejected"
                with self.assertRaises(ValueError):
                    FETCH.safe_extract(archive, destination)
                self.assertFalse(destination.exists())
                self.assertFalse((self.base / "escape.txt").exists())
        with mock.patch.object(FETCH.ntpath, "isreserved", return_value=True, create=True) as modern:
            with mock.patch.object(FETCH.PureWindowsPath, "is_reserved", side_effect=AssertionError("Legacy API was used"), create=True):
                with self.assertRaises(ValueError):
                    FETCH.safe_parts("NUL .txt")
            modern.assert_called_once_with("NUL .txt")

    def test_rejects_symlinks_duplicate_case_and_multiple_roots(self):
        link = zipfile.ZipInfo("package/link")
        link.create_system = 3
        link.external_attr = (stat.S_IFLNK | 0o777) << 16
        for entries in [[(link, "../../escape")], [("package/a", "a"), ("package/A", "b")],
                        [("one/a", "a"), ("two/b", "b")],
                        [("package/Include/a", "a"), ("package/include/b", "b")]]:
            archive = self.archive(entries)
            with self.assertRaises(ValueError):
                FETCH.safe_extract(archive, self.base / "rejected")
            self.assertFalse((self.base / "rejected").exists())

    def test_rejects_expansion_and_member_count_before_extraction(self):
        archive = self.archive([("package/a", "123456"), ("package/b", "b")])
        old_bytes, old_files = FETCH.MAX_EXPANDED, FETCH.MAX_FILES
        try:
            FETCH.MAX_EXPANDED = 5
            with self.assertRaises(ValueError):
                FETCH.safe_extract(archive, self.base / "bytes")
            FETCH.MAX_EXPANDED = old_bytes
            FETCH.MAX_FILES = 1
            with self.assertRaises(ValueError):
                FETCH.safe_extract(archive, self.base / "files")
            self.assertFalse((self.base / "bytes").exists())
            self.assertFalse((self.base / "files").exists())
        finally:
            FETCH.MAX_EXPANDED, FETCH.MAX_FILES = old_bytes, old_files

    def test_rejects_wrong_size_hash_and_offline_missing_archives(self):
        archive = self.archive([("package/a", "a")])
        asset = {"name": "fixture", "archive": "fixture.zip", "bytes": archive.stat().st_size,
                 "sha256": FETCH.sha256(archive)}
        FETCH.check_archive(archive, asset)
        with self.assertRaises(ValueError):
            FETCH.check_archive(archive, {**asset, "bytes": asset["bytes"] + 1})
        with self.assertRaises(ValueError):
            FETCH.check_archive(archive, {**asset, "sha256": "0" * 64})
        cache = self.base / "cache"
        cache.mkdir()
        with self.assertRaises(FileNotFoundError):
            FETCH.acquire_archive({**asset, "archive": "missing.zip"}, cache, None, True)

    def test_checks_patch_paths_before_invoking_git(self):
        patch = self.base / "unsafe.patch"
        patch.write_text("--- a/../../outside\n+++ b/../../outside\n@@ -1 +1 @@\n-a\n+b\n")
        with self.assertRaises(ValueError):
            FETCH.apply_patch(self.base, patch)

    def test_source_inventory_detects_mutation_and_does_not_ignore_nested_markers(self):
        source = self.base / "source"
        source.mkdir()
        (source / "code.cpp").write_text("before")
        before = FETCH.inventory(source)
        (source / "code.cpp").write_text("after")
        self.assertNotEqual(FETCH.inventory(source), before)
        nested = source / "nested"
        nested.mkdir()
        (nested / FETCH.STAMP).write_text("unmanaged")
        self.assertIn("nested/" + FETCH.STAMP, FETCH.inventory(source))


if __name__ == "__main__":
    unittest.main()
