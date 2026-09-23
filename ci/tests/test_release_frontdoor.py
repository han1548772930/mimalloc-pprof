"""Release attempt identity and preflight regressions for issue #444."""

from __future__ import annotations

import json
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from ci import release

SHA = "a" * 40


class ReleaseFrontdoorTests(unittest.TestCase):
    def test_source_version_requires_matching_lockfile(self) -> None:
        self.assertEqual(release.source_version(), "1.0.0")
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            crate = root / "rust/mimalloc-pprof"
            crate.mkdir(parents=True)
            (crate / "Cargo.toml").write_text(
                '[package]\nname = "mimalloc-pprof"\nversion = "1.0.1"\n'
            )
            (root / "rust/Cargo.lock").write_text(
                '[[package]]\nname = "mimalloc-pprof"\nversion = "1.0.0"\n'
            )
            with patch.object(release, "ROOT", root), self.assertRaises(release.ReleaseError):
                release.source_version()

    def test_directive_is_exact_and_full(self) -> None:
        directive = release.directive(444, "1.0.1", SHA)
        self.assertEqual(directive["tag"], "v1.0.1")
        self.assertEqual(directive["candidate_sha"], SHA)
        self.assertEqual(directive["mode"], "full")
        self.assertEqual(len(directive["assets"]), 5)
        with self.assertRaises(release.ReleaseError):
            release.directive(444, "1.0.1", "a" * 7)

    def test_existing_directive_cannot_be_retargeted(self) -> None:
        original = release.directive(444, "1.0.1", SHA)
        release.require_same_directive(original, original)
        with self.assertRaises(release.ReleaseError):
            release.require_same_directive(original, release.directive(444, "1.0.1", "b" * 40))

    def test_artifact_preflight_rejects_missing_oversize_and_hash_mismatch(self) -> None:
        directive = release.directive(444, "1.0.1", SHA)
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            for name in directive["assets"]:
                (directory / name).write_bytes(name.encode())
            info = release.inspect_artifacts(directory, directive)
            self.assertEqual(info["candidate_sha"], SHA)
            self.assertEqual(len(info["artifacts"]), 5)
            release.verify_info(directory, directive, info)
            info["artifacts"][0]["sha256"] = "0" * 64
            with self.assertRaises(release.ReleaseError):
                release.verify_info(directory, directive, info)
            with (
                patch.object(release, "MAX_ASSET_BYTES", 1),
                self.assertRaises(release.ReleaseError),
            ):
                release.inspect_artifacts(directory, directive)
            (directory / directive["assets"][0]).unlink()
            with self.assertRaises(release.ReleaseError):
                release.inspect_artifacts(directory, directive)

    def test_comment_round_trip_and_dry_run_has_no_worker_dispatch(self) -> None:
        directive = release.directive(444, "1.0.1", SHA)
        body = release.comment_body(directive, "ready-preflight")
        self.assertEqual(release.parse_directive(body), directive)
        self.assertIsNone(release.parse_directive("ordinary comment"))
        with (
            patch(
                "sys.argv",
                ["release.py", "start", "--issue", "444", "--candidate-sha", SHA, "--dry"],
            ),
            patch.object(release, "source_version", return_value="1.0.1"),
            patch.object(release, "issue_directives", return_value=[]),
            patch.object(release, "validate_candidate"),
            patch.object(release, "command") as run,
        ):
            self.assertEqual(release.main(), 0)
            run.assert_not_called()

    def test_issue_directive_ignores_untrusted_comments(self) -> None:
        value = release.directive(444, "1.0.1", SHA)
        body = release.comment_body(value, "ready")
        comments = [
            [
                {"body": body, "author_association": "NONE", "user": {"login": "outsider"}},
                {"body": body, "author_association": "OWNER", "user": {"login": "zackees"}},
            ]
        ]
        with patch.object(release, "command", return_value=json.dumps(comments)):
            self.assertEqual(release.issue_directives(444), [value])

    def test_workflow_has_issue_gate_before_every_build(self) -> None:
        import yaml

        workflow = yaml.safe_load(
            (Path(__file__).resolve().parents[2] / ".github/workflows/auto-release.yml").read_text()
        )
        jobs = workflow["jobs"]
        self.assertIn("validate-attempt", jobs)
        for job in ("build-and-package", "build-binaries"):
            self.assertIn("validate-attempt", jobs[job]["needs"])
        self.assertNotIn("push", workflow.get("on", workflow.get(True, {})))


if __name__ == "__main__":
    unittest.main()
