"""RED/GREEN contracts for #438's raw data and README figures."""

from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path
from typing import Any

from bench_arena_reclaim import METRICS, PHASES, svg, validate_report

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "ci/bench_arena_reclaim.py"


def fixture_data() -> dict[str, Any]:
    records: list[dict[str, Any]] = []
    for scenario in ("empty", "nonempty", "retained-id"):
        for repeat in range(3):
            for mode in ("purge-only", "reclaim"):
                samples: list[dict[str, Any]] = []
                for i, phase in enumerate(PHASES):
                    reclaimed = mode == "reclaim" and scenario == "empty" and phase == "reclaim"
                    sample = {
                        "phase": phase,
                        "wave": 0 if i == 0 else 1 if i < 5 else 2,
                        "elapsed_ms": 1.0,
                        "reclaim_requested": mode == "reclaim" and phase == "reclaim",
                        "reclaim_pass_ran": mode == "reclaim" and phase == "reclaim",
                        "purge_status": 0,
                        "arenas_reclaimed": 1 if reclaimed else 0,
                        "arena_reclaim_bytes": 100 * 1048576 if reclaimed else 0,
                        "arenas_kept": 0,
                        "subprocs_pending": 0,
                        "retained_id_checked": scenario == "retained-id",
                        "retained_id_valid": scenario == "retained-id",
                    }
                    if scenario == "retained-id" and mode == "reclaim" and phase == "reclaim":
                        sample["arenas_kept"] = 1
                    sample.update(dict.fromkeys(METRICS, 0))
                    sample["rss_bytes"] = (10 if reclaimed else 20) * 1048576
                    sample["private_bytes"] = (8 if reclaimed else 18) * 1048576
                    sample["allocator_reserved_bytes"] = (100 if reclaimed else 200) * 1048576
                    samples.append(sample)
                records.append(
                    {
                        "owner_gate": False,
                        "purge_decommits": "1",
                        "scenario": scenario,
                        "repeat": repeat,
                        "mode": mode,
                        "samples": samples,
                    }
                )
    return {
        "schema": "arena-reclaim-v1",
        "source_sha": "a" * 40,
        "host": {"platform": "fixture"},
        "records": records,
    }


def test_rejects_missing_reclaim_marker() -> None:
    data = fixture_data()
    del data["records"][1]["samples"][4]["reclaim_pass_ran"]
    try:
        validate_report(data)
    except (ValueError, KeyError):
        return
    raise AssertionError("missing reclaim marker was accepted")


def test_figure_does_not_label_reservation_as_rss() -> None:
    chart = svg(fixture_data(), "tradeoff")
    assert "Additional RSS reduction" in chart
    assert "10.0 MiB" in chart
    assert "100 MiB" in chart
    assert "Virtual arena reservation released (not RSS)" in chart
    assert (
        "100.0 MiB"
        not in chart.split("Additional RSS reduction")[1].split("Additional private reduction")[0]
    )


def test_stale_render_is_red(tmp_path: Path) -> None:
    data = tmp_path / "results.json"
    data.write_text(json.dumps(fixture_data()), encoding="utf-8")
    command = [sys.executable, str(SCRIPT), "--data", str(data), "--out-dir", str(tmp_path)]
    subprocess.run([*command, "--render"], check=True)
    subprocess.run([*command, "--check"], check=True)
    (tmp_path / "arena-reclaim-timeline.svg").write_text("stale", encoding="utf-8")
    assert subprocess.run([*command, "--check"], capture_output=True).returncode != 0
