import argparse
import importlib.util
from pathlib import Path


def main():
    repository = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description="Prepare pinned OpenCV and STag dictionary sources.")
    parser.add_argument("--output-dir", type=Path, default=repository / "build/vision-deps")
    parser.add_argument("--archive-dir", type=Path)
    parser.add_argument("--offline", action="store_true")
    parser.add_argument("--verify-only", action="store_true")
    arguments = parser.parse_args()
    specification = importlib.util.spec_from_file_location("lyflow_fixed_dependencies", repository / "scripts/fetch-matching-deps.py")
    dependencies = importlib.util.module_from_spec(specification)
    specification.loader.exec_module(dependencies)
    dependencies.prepare(repository / "packaging/vision-deps.json", arguments.output_dir,
                         arguments.archive_dir.resolve() if arguments.archive_dir else None,
                         arguments.offline, arguments.verify_only,
                         expected_names={"opencv", "stag"}, max_archive_bytes=128 * 1024 * 1024)


if __name__ == "__main__":
    main()
