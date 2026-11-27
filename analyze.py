#!/usr/bin/env python3
"""Анализ CSV-файлов, полученных от десяти UDP-узлов."""

from __future__ import annotations

import argparse
import csv
import math
import statistics
from collections import defaultdict
from pathlib import Path

METRICS = ("total_cycle_ns", "send_ns", "processing_ns", "receive_ns")


def read_rows(input_directory: Path) -> list[dict[str, int]]:
    rows: list[dict[str, int]] = []
    for path in sorted(input_directory.glob("node_*.csv")):
        with path.open("r", encoding="utf-8-sig", newline="") as source:
            for raw in csv.DictReader(source):
                rows.append({key: int(value) for key, value in raw.items()})
    if not rows:
        raise RuntimeError(f"В каталоге {input_directory} нет файлов node_*.csv")
    return rows


def build_system_rows(rows: list[dict[str, int]]) -> list[dict[str, int]]:
    grouped: dict[int, list[dict[str, int]]] = defaultdict(list)
    for row in rows:
        grouped[row["cycle_id"]].append(row)

    system_rows: list[dict[str, int]] = []
    for cycle_id, cycle_rows in sorted(grouped.items()):
        node_one = next((row for row in cycle_rows if row["node_id"] == 1), None)
        if node_one is None:
            continue
        system_rows.append({
            "cycle_id": cycle_id,
            "node_count": len({row["node_id"] for row in cycle_rows}),
            "payload_bytes": node_one["payload_bytes"],
            "total_cycle_ns": node_one["total_cycle_ns"],
            "send_ns": sum(row["send_ns"] for row in cycle_rows),
            "processing_ns": sum(row["processing_ns"] for row in cycle_rows),
            "receive_ns": sum(row["receive_ns"] for row in cycle_rows),
            "complete": int(
                len(cycle_rows) == 10 and all(row["complete"] for row in cycle_rows)
            ),
        })
    return system_rows


def write_system_cycles(path: Path, system_rows: list[dict[str, int]]) -> None:
    fields = [
        "cycle_id", "node_count", "payload_bytes", "total_cycle_ns",
        "send_ns", "processing_ns", "receive_ns", "complete",
    ]
    with path.open("w", encoding="utf-8", newline="") as target:
        writer = csv.DictWriter(target, fieldnames=fields)
        writer.writeheader()
        writer.writerows(system_rows)


def describe(values: list[int]) -> tuple[float, float, float]:
    values_us = [value / 1000.0 for value in values]
    return min(values_us), max(values_us), statistics.fmean(values_us)


def write_summary(
    path: Path,
    rows: list[dict[str, int]],
    system_rows: list[dict[str, int]],
) -> None:
    by_node: dict[int, list[dict[str, int]]] = defaultdict(list)
    for row in rows:
        by_node[row["node_id"]].append(row)

    scopes: list[tuple[str, list[dict[str, int]]]] = [("system", system_rows)]
    scopes.extend(
        (f"node_{node_id}", values)
        for node_id, values in sorted(by_node.items())
    )

    with path.open("w", encoding="utf-8", newline="") as target:
        writer = csv.writer(target)
        writer.writerow(["scope", "metric", "count", "min_us", "max_us", "mean_us"])
        for scope, scope_rows in scopes:
            for metric in METRICS:
                values = [row[metric] for row in scope_rows]
                if not values:
                    continue
                minimum, maximum, mean = describe(values)
                writer.writerow([
                    scope, metric, len(values),
                    f"{minimum:.3f}", f"{maximum:.3f}", f"{mean:.3f}",
                ])


def histogram(values: list[int], bin_count: int) -> list[tuple[float, float, int]]:
    values_us = [value / 1000.0 for value in values]
    minimum, maximum = min(values_us), max(values_us)
    if math.isclose(minimum, maximum):
        return [(minimum, maximum, len(values_us))]
    width = (maximum - minimum) / bin_count
    counts = [0] * bin_count
    for value in values_us:
        index = min(int((value - minimum) / width), bin_count - 1)
        counts[index] += 1
    return [
        (minimum + index * width, minimum + (index + 1) * width, count)
        for index, count in enumerate(counts)
    ]


def write_histograms(
    path: Path,
    rows: list[dict[str, int]],
    system_rows: list[dict[str, int]],
    bin_count: int,
) -> None:
    by_node: dict[int, list[dict[str, int]]] = defaultdict(list)
    for row in rows:
        by_node[row["node_id"]].append(row)
    scopes: list[tuple[str, list[dict[str, int]]]] = [("system", system_rows)]
    scopes.extend(
        (f"node_{node_id}", values)
        for node_id, values in sorted(by_node.items())
    )

    with path.open("w", encoding="utf-8", newline="") as target:
        writer = csv.writer(target)
        writer.writerow(["scope", "metric", "bin_start_us", "bin_end_us", "count"])
        for scope, scope_rows in scopes:
            for metric in METRICS:
                values = [row[metric] for row in scope_rows]
                if not values:
                    continue
                for start, end, count in histogram(values, bin_count):
                    writer.writerow([
                        scope, metric, f"{start:.3f}", f"{end:.3f}", count,
                    ])


def create_plots(
    output_directory: Path,
    rows: list[dict[str, int]],
    system_rows: list[dict[str, int]],
    bin_count: int,
) -> bool:
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        return False

    cycles = [row["cycle_id"] for row in system_rows]
    total_us = [row["total_cycle_ns"] / 1000.0 for row in system_rows]

    plt.figure(figsize=(10, 5))
    plt.plot(cycles, total_us, linewidth=1, label="Полный цикл")
    plt.axhline(200.0, color="red", linestyle="--", label="Требование 200 мкс")
    plt.xlabel("Номер цикла")
    plt.ylabel("Время, мкс")
    plt.title("Время полного цикла системы")
    plt.grid(True, alpha=0.3)
    plt.legend()
    plt.tight_layout()
    plt.savefig(output_directory / "system_cycle_time.png", dpi=160)
    plt.close()

    plt.figure(figsize=(10, 5))
    plt.plot(cycles, [row["send_ns"] / 1000.0 for row in system_rows], label="Отправка")
    plt.plot(cycles, [row["processing_ns"] / 1000.0 for row in system_rows], label="Обработка")
    plt.plot(cycles, [row["receive_ns"] / 1000.0 for row in system_rows], label="recvfrom")
    plt.xlabel("Номер цикла")
    plt.ylabel("Суммарное время, мкс")
    plt.title("Составляющие времени системы")
    plt.grid(True, alpha=0.3)
    plt.legend()
    plt.tight_layout()
    plt.savefig(output_directory / "system_components.png", dpi=160)
    plt.close()

    by_node: dict[int, list[float]] = defaultdict(list)
    for row in rows:
        by_node[row["node_id"]].append(row["total_cycle_ns"] / 1000.0)
    node_ids = sorted(by_node)
    averages = [statistics.fmean(by_node[node_id]) for node_id in node_ids]
    plt.figure(figsize=(9, 5))
    plt.bar(node_ids, averages)
    plt.xlabel("Номер узла")
    plt.ylabel("Среднее время, мкс")
    plt.title("Среднее время цикла по узлам")
    plt.xticks(node_ids)
    plt.tight_layout()
    plt.savefig(output_directory / "node_average_cycle.png", dpi=160)
    plt.close()

    plt.figure(figsize=(9, 5))
    plt.hist(total_us, bins=bin_count, edgecolor="black")
    plt.axvline(200.0, color="red", linestyle="--", label="200 мкс")
    plt.xlabel("Время полного цикла, мкс")
    plt.ylabel("Количество измерений")
    plt.title("Гистограмма времени полного цикла")
    plt.legend()
    plt.tight_layout()
    plt.savefig(output_directory / "system_cycle_histogram.png", dpi=160)
    plt.close()
    return True


def main() -> None:
    parser = argparse.ArgumentParser(description="Анализ статистики UDP-обмена")
    parser.add_argument("input", type=Path, help="Каталог с файлами node_*.csv")
    parser.add_argument("--output", type=Path, default=Path("analysis"))
    parser.add_argument("--bins", type=int, default=20)
    arguments = parser.parse_args()
    if arguments.bins < 1:
        parser.error("--bins должен быть больше нуля")

    rows = read_rows(arguments.input)
    system_rows = build_system_rows(rows)
    if not system_rows:
        raise RuntimeError("Не найдены циклы со статистикой узла 1")

    arguments.output.mkdir(parents=True, exist_ok=True)
    write_system_cycles(arguments.output / "system_cycles.csv", system_rows)
    write_summary(arguments.output / "summary.csv", rows, system_rows)
    write_histograms(
        arguments.output / "histogram.csv", rows, system_rows, arguments.bins
    )
    plots_created = create_plots(
        arguments.output, rows, system_rows, arguments.bins
    )

    complete = sum(row["complete"] for row in system_rows)
    print(f"Обработано записей узлов: {len(rows)}")
    print(f"Системных циклов: {len(system_rows)}, полных: {complete}")
    print(f"Результаты: {arguments.output}")
    if not plots_created:
        print(
            "Графики не построены: установите matplotlib командой "
            "sudo apt install python3-matplotlib"
        )


if __name__ == "__main__":
    main()
