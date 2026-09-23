#!/usr/bin/env python3
"""Reproduce #438's paired arena-reclaim experiment and its sourced SVGs.

Measurement: --measure --build-root PATH --out-dir PATH [--runs 3]
Rendering:   --render --data PATH --out-dir PATH
Validation:  --check --data PATH --out-dir PATH (no build or measurement)
"""

from __future__ import annotations

import argparse
import hashlib
import itertools
import json
import os
import platform
import statistics
import subprocess
import sys
from pathlib import Path
from typing import Any
from xml.sax.saxutils import escape

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_DATA = ROOT / ".github/assets/arena-reclaim-results.json"
MODES = ("purge-only", "reclaim")
SCENARIOS = ("empty", "nonempty", "retained-id")
THREAD_STATES = ("none", "idle", "active")
PHASES = ("baseline", "peak", "free", "purge", "reclaim", "peak", "free", "purge", "reclaim")
METRICS = (
    "rss_bytes",
    "private_bytes",
    "process_virtual_bytes",
    "allocator_reserved_bytes",
    "allocator_committed_bytes",
    "arena_metadata_bytes",
)


def run(command: list[str], *, cwd: Path = ROOT) -> str:
    result = subprocess.run(command, cwd=cwd, text=True, capture_output=True)
    if result.returncode:
        raise RuntimeError(
            f"{command!r} failed ({result.returncode})\n{result.stdout}\n{result.stderr}"
        )
    return result.stdout


def build(root: Path, owner_gate: bool, toolchain: str) -> tuple[Path, list[str]]:
    directory = root / ("owner-gate-on" if owner_gate else "owner-gate-off")
    flags = [
        "-DCMAKE_BUILD_TYPE=Release",
        "-DMI_BUILD_TESTS=OFF",
        "-DMI_BUILD_SHARED=OFF",
        "-DMI_BUILD_OBJECT=OFF",
        "-DMI_PPROF=OFF",
        f"-DMI_OWNER_GATE={'ON' if owner_gate else 'OFF'}",
    ]
    generator = [] if toolchain == "msvc" else ["-G", "Ninja"]
    run(
        [
            "cmake",
            "-S",
            str(ROOT / "ci/arena_reclaim_bench"),
            "-B",
            str(directory),
            *generator,
            *flags,
        ]
    )
    run(
        [
            "cmake",
            "--build",
            str(directory),
            "--config",
            "Release",
            "--target",
            "bench-arena-reclaim",
            "-j",
            "4",
        ]
    )
    names = ("bench-arena-reclaim.exe", "bench-arena-reclaim")
    executables = [p for name in names for p in directory.rglob(name) if p.is_file()]
    if len(executables) != 1:
        raise ValueError(f"expected one benchmark executable in {directory}: {executables}")
    return executables[0], flags


def validate_samples(
    samples: list[dict[str, Any]], mode: str, scenario: str, thread_state: str
) -> None:
    if tuple(s["phase"] for s in samples) != PHASES or tuple(s["wave"] for s in samples) != (
        0,
        1,
        1,
        1,
        1,
        2,
        2,
        2,
        2,
    ):
        raise ValueError("missing or reordered phase/wave marker")
    for sample in samples:
        for metric in METRICS:
            if not isinstance(sample.get(metric), int) or sample[metric] < 0:
                raise ValueError(f"missing or invalid {metric}")
        if sample["reclaim_requested"] and sample["phase"] != "reclaim":
            raise ValueError("reclaim flag reported outside reclaim phase")
        if not sample["reclaim_requested"] and sample.get("reclaim_result") != "not-requested":
            raise ValueError("unexpected result for unrequested reclaim")
    reclaim_rows = (samples[4], samples[8])
    for row in reclaim_rows:
        if row["reclaim_requested"] != (mode == "reclaim"):
            raise ValueError("missing reclaim/no-op marker")
        if not isinstance(row["reclaim_pass_ran"], bool):
            raise ValueError("missing reclaim-pass marker")
        result = row.get("reclaim_result")
        expected_result = (
            "not-requested"
            if mode == "purge-only"
            else "purge-busy"
            if row["purge_status"] == 2
            else "pending-subprocess"
            if not row["reclaim_pass_ran"] and row["subprocs_pending"] > 0
            else "arena-layer-busy"
            if not row["reclaim_pass_ran"]
            else "released"
            if row["arenas_reclaimed"] > 0
            else "no-eligible-arena"
        )
        if result != expected_result:
            raise ValueError(f"missing or inconsistent reclaim/no-op reason: {result!r}")
        if mode == "purge-only" and (row["reclaim_pass_ran"] or row["arena_reclaim_bytes"]):
            raise ValueError("purge-only control reports reclamation")
        if row["arena_reclaim_bytes"] > 0 and not row["reclaim_pass_ran"]:
            raise ValueError("reclaimed bytes without a completed pass")
        if scenario == "nonempty" and row["arenas_reclaimed"] > 0:
            raise ValueError("nonempty control released a live arena")
        if (
            scenario == "retained-id"
            and mode == "reclaim"
            and row["reclaim_pass_ran"]
            and row["arenas_kept"] < 1
        ):
            raise ValueError("caller-reserved arena was not reported kept")
    if scenario == "retained-id" and any(
        s.get("retained_id_checked") is not True or s.get("retained_id_valid") is not True
        for s in samples
    ):
        raise ValueError("retained arena ID did not survive the pass")
    if any(s.get("worker_started") is not (thread_state != "none") for s in samples):
        raise ValueError("worker state did not match the requested condition")


def measure(
    build_root: Path, runs: int, decommits: tuple[str, ...], toolchain: str
) -> dict[str, Any]:
    if runs < 3:
        raise ValueError("at least three paired repetitions are required")
    sha = os.environ.get("GITHUB_SHA") or run(["git", "rev-parse", "HEAD"]).strip()
    report: dict[str, Any] = {
        "schema": "arena-reclaim-v1",
        "source_sha": sha,
        "benchmark_source_sha256": {
            name: hashlib.sha256((ROOT / name).read_bytes()).hexdigest()
            for name in (
                "ci/bench_arena_reclaim.c",
                "ci/bench_arena_reclaim.py",
                "ci/arena_reclaim_bench/CMakeLists.txt",
            )
        },
        "host": {
            "platform": platform.platform(),
            "node": platform.node(),
            "processor": platform.processor(),
            "machine": platform.machine(),
            "release": platform.release(),
            "python": sys.version.split()[0],
            "toolchain": toolchain,
            "runner_os": os.environ.get("RUNNER_OS", "local"),
            "runner_name": os.environ.get("RUNNER_NAME", "local"),
        },
        "workload": {
            "big_objects": 8,
            "object_bytes": 32 * 1024 * 1024,
            "waves": 2,
            "seed": 0,
            "touch_stride_bytes": 4096,
            "arena_reserve_kib": 32 * 1024,
        },
        "selection_rule": "per-phase arm medians; tradeoffs use median paired per-repeat deltas; no runs excluded",
        "records": [],
    }
    for owner_gate in (False, True):
        exe, flags = build(build_root, owner_gate, toolchain)
        for decommit in decommits:
            for scenario in SCENARIOS:
                for thread_state in THREAD_STATES:
                    for repeat in range(runs):
                        # Alternate order to reduce systematic cache/temperature bias.
                        modes = MODES if repeat % 2 == 0 else MODES[::-1]
                        for mode in modes:
                            env = os.environ.copy()
                            env["MIMALLOC_PURGE_DECOMMITS"] = decommit
                            command = [str(exe), mode, scenario, thread_state]
                            child = subprocess.run(
                                command,
                                cwd=ROOT,
                                env=env,
                                text=True,
                                capture_output=True,
                                timeout=180,
                            )
                            if child.returncode:
                                raise RuntimeError(
                                    f"{command!r} failed ({child.returncode})\n{child.stdout}\n{child.stderr}"
                                )
                            samples = [
                                json.loads(line)
                                for line in child.stdout.splitlines()
                                if line.startswith("{")
                            ]
                            validate_samples(samples, mode, scenario, thread_state)
                            report["records"].append(
                                {
                                    "owner_gate": owner_gate,
                                    "purge_decommits": decommit,
                                    "scenario": scenario,
                                    "thread_state": thread_state,
                                    "repeat": repeat,
                                    "mode": mode,
                                    "cmake_flags": flags,
                                    "command": command,
                                    "samples": samples,
                                }
                            )
    return report


def validate_report(data: dict[str, Any]) -> None:
    if data.get("schema") != "arena-reclaim-v1" or not isinstance(data.get("records"), list):
        raise ValueError("invalid arena-reclaim data schema")
    if len(data["records"]) < 3 * 2 * 2 * len(SCENARIOS) * len(THREAD_STATES) * len(MODES):
        raise ValueError("insufficient raw paired repetitions")
    grouped: dict[tuple[bool, str, str, str, int], set[str]] = {}
    seen: set[tuple[bool, str, str, str, int, str]] = set()
    repeats: set[int] = set()
    for record in data["records"]:
        mode, scenario = record["mode"], record["scenario"]
        thread_state = record["thread_state"]
        if mode not in MODES or scenario not in SCENARIOS or thread_state not in THREAD_STATES:
            raise ValueError("unknown mode/scenario")
        if type(record["owner_gate"]) is not bool or record["purge_decommits"] not in ("0", "1"):
            raise ValueError("invalid owner-gate/decommit configuration")
        if type(record["repeat"]) is not int or record["repeat"] < 0:
            raise ValueError("invalid repeat index")
        repeats.add(record["repeat"])
        validate_samples(record["samples"], mode, scenario, thread_state)
        if (
            thread_state == "idle"
            and not record["owner_gate"]
            and not all(s.get("worker_parked") is True for s in record["samples"])
        ):
            raise ValueError("ungated idle worker did not park")
        key = (
            record["owner_gate"],
            record["purge_decommits"],
            scenario,
            thread_state,
            record["repeat"],
        )
        if (*key, mode) in seen:
            raise ValueError("duplicate paired arm")
        seen.add((*key, mode))
        grouped.setdefault(key, set()).add(mode)
    if any(modes != set(MODES) for modes in grouped.values()):
        raise ValueError("unpaired run")
    if len(repeats) < 3 or repeats != set(range(len(repeats))):
        raise ValueError("expected at least three consecutive raw repetitions")
    expected = set(itertools.product((False, True), ("0", "1"), SCENARIOS, THREAD_STATES, repeats))
    if set(grouped) != expected:
        raise ValueError("missing owner-gate/decommit/scenario/thread/repetition arm")


def svg(data: dict[str, Any], kind: str) -> str:
    """Small deterministic chart: one Linux/Windows panel per committed dataset."""
    validate_report(data)
    records = [
        r
        for r in data["records"]
        if not r["owner_gate"]
        and r["purge_decommits"] == "1"
        and r["scenario"] == "empty"
        and r["thread_state"] == "none"
    ]
    if not records:
        raise ValueError("no default-build empty-arena series")
    pairs: dict[int, dict[str, dict[str, Any]]] = {}
    for record in records:
        pairs.setdefault(record["repeat"], {})[record["mode"]] = record

    def paired_difference(metric: str, phase_index: int) -> float:
        return statistics.median(
            float(pair["purge-only"]["samples"][phase_index][metric])
            - float(pair["reclaim"]["samples"][phase_index][metric])
            for pair in pairs.values()
        )

    def reclaim_median(metric: str, phase_index: int) -> float:
        return statistics.median(
            float(pair["reclaim"]["samples"][phase_index][metric]) for pair in pairs.values()
        )

    nonempty = [
        r["samples"][4]
        for r in data["records"]
        if not r["owner_gate"]
        and r["purge_decommits"] == "1"
        and r["scenario"] == "nonempty"
        and r["thread_state"] == "none"
        and r["mode"] == "reclaim"
    ]
    active = [
        r["samples"][4]
        for r in data["records"]
        if not r["owner_gate"]
        and r["purge_decommits"] == "1"
        and r["scenario"] == "empty"
        and r["thread_state"] == "active"
        and r["mode"] == "reclaim"
    ]
    no_op_note = (
        f"Nonempty no-eligible: {sum(s['reclaim_result'] == 'no-eligible-arena' for s in nonempty)}/{len(nonempty)}; "
        f"active-worker pending: {sum(s['reclaim_result'] == 'pending-subprocess' for s in active)}/{len(active)}."
    )
    labels = [
        "baseline",
        "peak",
        "free",
        "purge",
        "reclaim",
        "regrowth",
        "free",
        "purge",
        "reclaim",
    ]
    medians: dict[str, list[dict[str, float]]] = {}
    for mode in MODES:
        cohort = [r["samples"] for r in records if r["mode"] == mode]
        if len(cohort) < 3:
            raise ValueError("fewer than 3 repetitions for chart")
        medians[mode] = [
            {
                key: statistics.median(float(run[i][key]) for run in cohort)
                for key in (*METRICS, "elapsed_ms", "arena_reclaim_bytes")
            }
            for i in range(9)
        ]
    lines = [
        '<svg xmlns="http://www.w3.org/2000/svg" width="960" height="460" viewBox="0 0 960 460">',
        '<rect width="960" height="460" fill="#fff"/>',
    ]
    title = (
        "Arena reclaim: physical memory by phase"
        if kind == "timeline"
        else "Arena reclaim: cost and incremental savings"
    )
    lines.append(f'<text x="36" y="36" font-size="22" font-family="sans-serif">{title}</text>')
    lines.append(
        f'<text x="36" y="58" font-size="12" font-family="sans-serif">{escape(data["host"]["platform"])} · {escape(data["source_sha"][:12])} · median of {len(records) // 2} paired runs</text>'
    )
    if kind == "timeline":
        maximum = max(x["rss_bytes"] for series in medians.values() for x in series) / 1048576
        maximum = max(maximum, 1)
        for y, mb in ((80, maximum), (390, 0)):
            lines.append(
                f'<text x="20" y="{y}" font-size="12" font-family="sans-serif">{mb:.0f} MiB RSS</text>'
            )
        for mode, color in (("purge-only", "#9a4a00"), ("reclaim", "#075aa6")):
            coords = " ".join(
                f"{105 + i * 100},{390 - 310 * sample['rss_bytes'] / 1048576 / maximum:.1f}"
                for i, sample in enumerate(medians[mode])
            )
            lines.append(
                f'<polyline points="{coords}" fill="none" stroke="{color}" stroke-width="3"/>'
            )
        for i, label in enumerate(labels):
            lines.append(
                f'<text x="{105 + i * 100}" y="418" text-anchor="middle" font-size="11" font-family="sans-serif">{label}</text>'
            )
        lines.append(
            '<text x="90" y="83" fill="#9a4a00" font-size="13" font-family="sans-serif">purge only</text><text x="200" y="83" fill="#075aa6" font-size="13" font-family="sans-serif">explicit reclaim</text>'
        )
        lines.append(
            '<text x="36" y="440" font-size="12" font-family="sans-serif">RSS is physical; allocator-reserved virtual address space is NOT RSS.</text>'
        )
        lines.append(
            f'<text x="36" y="455" font-size="11" font-family="sans-serif">{escape(no_op_note)}</text>'
        )
    else:
        saved_rss = paired_difference("rss_bytes", 4) / 1048576
        saved_private = paired_difference("private_bytes", 4) / 1048576
        saved_committed = paired_difference("allocator_committed_bytes", 4) / 1048576
        reserved = reclaim_median("arena_reclaim_bytes", 4) / 1048576
        regrowth_extra = -paired_difference("elapsed_ms", 5)
        costs = [
            ("Additional RSS reduction", f"{saved_rss:.1f} MiB", "#075aa6"),
            ("Additional private reduction", f"{saved_private:.1f} MiB", "#075aa6"),
            ("Additional allocator commit reduction", f"{saved_committed:.1f} MiB", "#075aa6"),
            ("Virtual arena reservation released (not RSS)", f"{reserved:.0f} MiB", "#777"),
            ("Reclaim call median", f"{reclaim_median('elapsed_ms', 4):.2f} ms", "#9a4a00"),
            ("Regrowth extra time vs paired control", f"{regrowth_extra:+.2f} ms", "#9a4a00"),
        ]
        for i, (label, value, color) in enumerate(costs):
            y = 100 + i * 48
            lines.append(
                f'<text x="45" y="{y}" font-size="16" font-family="sans-serif">{label}</text><text x="780" y="{y}" fill="{color}" font-size="19" font-family="sans-serif">{value}</text>'
            )
        lines.append(
            '<text x="36" y="410" font-size="12" font-family="sans-serif">All reductions are additional to ordinary purge; virtual reservation is not physical memory.</text>'
        )
        lines.append(
            f'<text x="36" y="432" font-size="11" font-family="sans-serif">{escape(no_op_note)}</text>'
        )
    lines.append("</svg>")
    return "\n".join(lines) + "\n"


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    operation = parser.add_mutually_exclusive_group(required=True)
    operation.add_argument("--measure", action="store_true")
    operation.add_argument("--render", action="store_true")
    operation.add_argument("--check", action="store_true")
    parser.add_argument("--build-root", type=Path)
    parser.add_argument("--data", type=Path, default=DEFAULT_DATA)
    parser.add_argument("--out-dir", type=Path, default=ROOT / ".github/assets")
    parser.add_argument("--runs", type=int, default=3)
    parser.add_argument("--decommits", choices=("1", "0", "both"), default="both")
    parser.add_argument(
        "--toolchain",
        choices=("unix", "msvc", "mingw"),
        default="msvc" if os.name == "nt" else "unix",
    )
    args = parser.parse_args()
    if args.measure:
        if args.build_root is None:
            parser.error("--measure requires --build-root")
        data = measure(
            args.build_root,
            args.runs,
            ("1", "0") if args.decommits == "both" else (args.decommits,),
            args.toolchain,
        )
        validate_report(data)
        args.data.parent.mkdir(parents=True, exist_ok=True)
        args.data.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        return
    data = json.loads(args.data.read_text(encoding="utf-8"))
    validate_report(data)
    for kind in ("timeline", "tradeoff"):
        path = args.out_dir / f"arena-reclaim-{kind}.svg"
        rendered = svg(data, kind)
        if args.check:
            if path.read_text(encoding="utf-8") != rendered:
                raise ValueError(f"stale chart: {path}")
        else:
            args.out_dir.mkdir(parents=True, exist_ok=True)
            path.write_text(rendered, encoding="utf-8")


if __name__ == "__main__":
    main()
