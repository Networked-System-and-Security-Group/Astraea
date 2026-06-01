#!/usr/bin/env python3
"""Count physical lines in Astraea source files.

The count is intentionally simple: every line in every regular file under
src/ is counted, including blank lines and comments.
"""

from __future__ import annotations

import argparse
from collections import defaultdict
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]


def count_lines(path: Path) -> int:
    with path.open("r", encoding="utf-8", errors="replace") as f:
        return sum(1 for _ in f)


def collect_files(src_dir: Path) -> list[Path]:
    return sorted(path for path in src_dir.rglob("*") if path.is_file())


def format_table(rows: list[tuple[str, int]]) -> str:
    path_width = max([len("file"), *(len(path) for path, _ in rows)])
    line_width = max([len("lines"), *(len(str(lines)) for _, lines in rows)])

    output = [
        f"{'file':<{path_width}}  {'lines':>{line_width}}",
        f"{'-' * path_width}  {'-' * line_width}",
    ]
    output.extend(f"{path:<{path_width}}  {lines:>{line_width}}" for path, lines in rows)
    return "\n".join(output)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--src-dir",
        type=Path,
        default=REPO_ROOT / "src",
        help="source directory to count, default: src",
    )
    parser.add_argument(
        "--total-only",
        action="store_true",
        help="print only the total line count",
    )
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    src_dir = args.src_dir.resolve()

    if not src_dir.is_dir():
        raise SystemExit(f"source directory not found: {src_dir}")

    files = collect_files(src_dir)
    if not files:
        raise SystemExit(f"no files found under {src_dir}")

    rows: list[tuple[str, int]] = []
    subtotals: dict[str, int] = defaultdict(int)

    for path in files:
        lines = count_lines(path)
        rel_path = path.relative_to(REPO_ROOT) if path.is_relative_to(REPO_ROOT) else path
        rel_name = rel_path.as_posix()
        rows.append((rel_name, lines))

        try:
            subdir = path.relative_to(src_dir).parts[0]
        except IndexError:
            subdir = "."
        subtotals[subdir] += lines

    total = sum(lines for _, lines in rows)

    if args.total_only:
        print(total)
        return

    print(format_table(rows))
    print()
    print("subtotals:")
    for name in sorted(subtotals):
        print(f"  {name}: {subtotals[name]}")
    print()
    print(f"files: {len(rows)}")
    print(f"total lines: {total}")


if __name__ == "__main__":
    main()
