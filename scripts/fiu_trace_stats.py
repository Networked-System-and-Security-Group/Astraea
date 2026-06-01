#!/usr/bin/env python3
"""Summarize FIU trace request sizes.

The FIU .revised traces are whitespace-separated. Request size is column 4
and is measured in 512-byte sectors.
"""

from __future__ import annotations

import argparse
import csv
from collections import Counter
from pathlib import Path

import pandas as pd


SECTOR_BYTES = 512
CHUNK_ROWS = 1_000_000


def quantile_from_hist(hist: Counter[int], count: int, q: float) -> int:
    if count == 0:
        return 0

    rank = int((count - 1) * q)
    seen = 0
    for value in sorted(hist):
        seen += hist[value]
        if seen > rank:
            return value
    return max(hist)


def summarize_trace(path: Path) -> dict[str, object]:
    total_count = 0
    write_count = 0
    read_count = 0
    total_sectors = 0
    write_sectors = 0
    read_sectors = 0
    hist: Counter[int] = Counter()
    write_hist: Counter[int] = Counter()
    read_hist: Counter[int] = Counter()

    reader = pd.read_csv(
        path,
        sep=r"\s+",
        header=None,
        usecols=[1, 3],
        names=["op", "sectors"],
        chunksize=CHUNK_ROWS,
        engine="c",
    )

    for chunk in reader:
        chunk["sectors"] = pd.to_numeric(chunk["sectors"], errors="coerce")
        chunk = chunk.dropna(subset=["sectors"])
        chunk["sectors"] = chunk["sectors"].astype("int64")

        counts = chunk["sectors"].value_counts()
        for sectors, count in counts.items():
            hist[int(sectors)] += int(count)

        total_count += int(len(chunk))
        total_sectors += int(chunk["sectors"].sum())

        writes = chunk[chunk["op"] == "WS"]
        write_count += int(len(writes))
        write_sectors += int(writes["sectors"].sum())
        write_counts = writes["sectors"].value_counts()
        for sectors, count in write_counts.items():
            write_hist[int(sectors)] += int(count)

        reads = chunk[chunk["op"] == "RS"]
        read_count += int(len(reads))
        read_sectors += int(reads["sectors"].sum())
        read_counts = reads["sectors"].value_counts()
        for sectors, count in read_counts.items():
            read_hist[int(sectors)] += int(count)

    avg_kib = (total_sectors * SECTOR_BYTES / 1024 / total_count) if total_count else 0.0
    write_avg_kib = (write_sectors * SECTOR_BYTES / 1024 / write_count) if write_count else 0.0
    read_avg_kib = (read_sectors * SECTOR_BYTES / 1024 / read_count) if read_count else 0.0

    return {
        "trace": path.name,
        "requests": total_count,
        "write_requests": write_count,
        "read_requests": read_count,
        "write_ratio": write_count / total_count if total_count else 0.0,
        "avg_kib": avg_kib,
        "write_avg_kib": write_avg_kib,
        "read_avg_kib": read_avg_kib,
        "p50_kib": quantile_from_hist(hist, total_count, 0.50) * SECTOR_BYTES / 1024,
        "p90_kib": quantile_from_hist(hist, total_count, 0.90) * SECTOR_BYTES / 1024,
        "p95_kib": quantile_from_hist(hist, total_count, 0.95) * SECTOR_BYTES / 1024,
        "p99_kib": quantile_from_hist(hist, total_count, 0.99) * SECTOR_BYTES / 1024,
        "max_kib": (max(hist) * SECTOR_BYTES / 1024) if hist else 0.0,
        "write_p50_kib": quantile_from_hist(write_hist, write_count, 0.50) * SECTOR_BYTES / 1024,
        "write_p90_kib": quantile_from_hist(write_hist, write_count, 0.90) * SECTOR_BYTES / 1024,
        "write_p95_kib": quantile_from_hist(write_hist, write_count, 0.95) * SECTOR_BYTES / 1024,
        "write_p99_kib": quantile_from_hist(write_hist, write_count, 0.99) * SECTOR_BYTES / 1024,
        "write_max_kib": (max(write_hist) * SECTOR_BYTES / 1024) if write_hist else 0.0,
        "read_p50_kib": quantile_from_hist(read_hist, read_count, 0.50) * SECTOR_BYTES / 1024,
        "read_p90_kib": quantile_from_hist(read_hist, read_count, 0.90) * SECTOR_BYTES / 1024,
        "read_p95_kib": quantile_from_hist(read_hist, read_count, 0.95) * SECTOR_BYTES / 1024,
        "read_p99_kib": quantile_from_hist(read_hist, read_count, 0.99) * SECTOR_BYTES / 1024,
        "read_max_kib": (max(read_hist) * SECTOR_BYTES / 1024) if read_hist else 0.0,
        "total_gib": total_sectors * SECTOR_BYTES / 1024 / 1024 / 1024,
    }


def write_csv(rows: list[dict[str, object]], output: Path) -> None:
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
        writer.writeheader()
        writer.writerows(rows)


def print_top(rows: list[dict[str, object]], limit: int, min_avg_kib: float) -> None:
    eligible = [row for row in rows if float(row["avg_kib"]) >= min_avg_kib]
    eligible.sort(key=lambda row: (float(row["avg_kib"]), int(row["requests"])), reverse=True)

    print(f"Top traces by average request size (avg_kib >= {min_avg_kib:g}):")
    print(
        "trace,requests,write_ratio,avg_kib,write_avg_kib,p50_kib,p95_kib,p99_kib,max_kib,total_gib"
    )
    for row in eligible[:limit]:
        print(
            f"{row['trace']},{row['requests']},{float(row['write_ratio']):.3f},"
            f"{float(row['avg_kib']):.2f},{float(row['write_avg_kib']):.2f},"
            f"{float(row['p50_kib']):.2f},{float(row['p95_kib']):.2f},"
            f"{float(row['p99_kib']):.2f},{float(row['max_kib']):.2f},"
            f"{float(row['total_gib']):.2f}"
        )


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--trace-dir", type=Path, default=Path("data/fiu"))
    parser.add_argument("--output", type=Path, default=Path("data/fiu/fiu_trace_stats.csv"))
    parser.add_argument("--limit", type=int, default=30)
    parser.add_argument("--min-avg-kib", type=float, default=1.0)
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    traces = sorted(args.trace_dir.glob("*.revised"))
    if not traces:
        raise SystemExit(f"no .revised files found under {args.trace_dir}")

    rows = [summarize_trace(path) for path in traces]
    rows.sort(key=lambda row: row["trace"])
    write_csv(rows, args.output)
    print(f"Wrote {len(rows)} rows to {args.output}")
    print_top(rows, args.limit, args.min_avg_kib)


if __name__ == "__main__":
    main()
