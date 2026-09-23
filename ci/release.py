#!/usr/bin/env python3
"""Issue-driven, SHA-pinned release entry point for mimalloc-pprof (#444).

The real publisher remains disabled in auto-release.yml until the destination
state machine is implemented. This front door only records an attempt and
dispatches a non-publishing worker.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, cast

ROOT = Path(__file__).resolve().parents[1]
REPO = "zackees/mimalloc-pprof"
MARKER = "<!-- fleet-release-attempt/v1 -->"
SHA_RE = re.compile(r"[0-9a-f]{40}\Z")
MAX_ASSET_BYTES = 100_000_000
ASSET_TEMPLATES = (
    "mimalloc-pprof-c-{tag}.zip",
    "mimalloc-pprof-macos-arm64-{tag}.tar.gz",
    "mimalloc-pprof-macos-x86_64-{tag}.tar.gz",
    "mimalloc-pprof-windows-x64-gnu-{tag}.zip",
    "mimalloc-pprof-windows-x64-msvc-{tag}.zip",
)


class ReleaseError(ValueError):
    """A candidate or release artifact is unsafe to proceed with."""


def command(*args: str) -> str:
    result = subprocess.run(args, cwd=ROOT, capture_output=True, text=True, check=False)
    if result.returncode:
        raise ReleaseError(f"{' '.join(args[:3])} failed: {result.stderr.strip()}")
    return result.stdout.strip()


def source_version() -> str:
    def field(section: str, name: str) -> str:
        match = re.search(rf'^\s*{re.escape(name)}\s*=\s*"([^"]+)"\s*$', section, re.MULTILINE)
        if not match:
            raise ReleaseError(f"missing {name} in Cargo package section")
        return match.group(1)

    manifest = (ROOT / "rust/mimalloc-pprof/Cargo.toml").read_text()
    package = re.search(r"(?ms)^\[package\]\s*\n(.*?)(?=^\[|\Z)", manifest)
    if not package:
        raise ReleaseError("Cargo.toml has no [package] section")
    version = field(package.group(1), "version")
    lock = (ROOT / "rust/Cargo.lock").read_text()
    sections = re.findall(r"(?ms)^\[\[package\]\]\s*\n(.*?)(?=^\[\[package\]\]|\Z)", lock)
    locked = [
        field(section, "version")
        for section in sections
        if field(section, "name") == "mimalloc-pprof"
    ]
    if locked != [version]:
        raise ReleaseError(
            f"Cargo.lock mimalloc-pprof version {locked} differs from Cargo.toml {version}"
        )
    return version


def directive(issue: int, version: str, candidate_sha: str) -> dict[str, Any]:
    if issue < 1 or not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", version):
        raise ReleaseError("issue and semver version are required")
    if not SHA_RE.fullmatch(candidate_sha):
        raise ReleaseError("candidate_sha must be a lowercase full 40-character SHA")
    tag = f"v{version}"
    return {
        "schema": "fleet-release-attempt/v1",
        "repository": REPO,
        "issue": issue,
        "version": version,
        "tag": tag,
        "candidate_sha": candidate_sha,
        "mode": "full",
        "destinations": ["crates.io", "github-release"],
        "assets": [template.format(tag=tag) for template in ASSET_TEMPLATES],
    }


def require_same_directive(existing: dict[str, Any], desired: dict[str, Any]) -> None:
    if existing != desired:
        raise ReleaseError("release directive is already bound to different identity or scope")


def comment_body(value: dict[str, Any], state: str, run_url: str = "") -> str:
    now = datetime.now(timezone.utc).isoformat()
    link = f"; run: {run_url}" if run_url else ""
    return f"{MARKER}\n```json\n{json.dumps(value, indent=2, sort_keys=True)}\n```\nState: {state}; UTC: {now}{link}"


def parse_directive(body: str) -> dict[str, Any] | None:
    if MARKER not in body:
        return None
    match = re.search(r"```json\n(.*?)\n```", body, re.DOTALL)
    if not match:
        raise ReleaseError("release comment has no JSON directive")
    value: object = json.loads(match.group(1))
    if not isinstance(value, dict):
        raise ReleaseError("unrecognized release directive")
    parsed = cast(dict[str, Any], value)
    if parsed.get("schema") != "fleet-release-attempt/v1":
        raise ReleaseError("unrecognized release directive")
    return parsed


def issue_comments(issue: int) -> list[str]:
    raw = command(
        "gh", "api", "--paginate", "--slurp", f"repos/{REPO}/issues/{issue}/comments?per_page=100"
    )
    pages = cast(list[list[dict[str, Any]]], json.loads(raw))
    trusted = {"OWNER", "MEMBER", "COLLABORATOR"}
    return [
        str(row["body"])
        for page in pages
        for row in page
        if row.get("author_association") in trusted
        or row.get("user", {}).get("login") == "github-actions[bot]"
    ]


def issue_directives(issue: int) -> list[dict[str, Any]]:
    return [parsed for body in issue_comments(issue) if (parsed := parse_directive(body))]


def validate_candidate(value: dict[str, Any], *, require_registry_free: bool = True) -> None:
    expected = directive(value["issue"], value["version"], value["candidate_sha"])
    require_same_directive(expected, value)
    issue = json.loads(
        command("gh", "issue", "view", str(value["issue"]), "-R", REPO, "--json", "title,state")
    )
    if issue.get("state") != "OPEN" or f"v{value['version']}" not in issue.get("title", ""):
        raise ReleaseError("release issue is closed or targets a different version")
    if source_version() != value["version"]:
        raise ReleaseError("source Cargo version differs from the release directive")
    head = command("git", "rev-parse", "HEAD")
    if head != value["candidate_sha"]:
        raise ReleaseError("checkout HEAD differs from release candidate SHA")
    command("git", "fetch", "origin", "main")
    if subprocess.run(
        ("git", "merge-base", "--is-ancestor", head, "origin/main"),
        cwd=ROOT,
        capture_output=True,
        check=False,
    ).returncode:
        raise ReleaseError("candidate is not merged into main")
    if command("git", "status", "--porcelain"):
        raise ReleaseError("release candidate checkout must be clean")
    existing_tag = command("git", "ls-remote", "--tags", "origin", f"refs/tags/{value['tag']}")
    if existing_tag:
        raise ReleaseError(f"tag {value['tag']} already exists")
    if require_registry_free:
        # gh api cannot query crates.io. urllib returns HTTP 404 for an unused version.
        import urllib.error
        import urllib.request

        url = f"https://crates.io/api/v1/crates/mimalloc-pprof/{value['version']}"
        request = urllib.request.Request(
            url, headers={"User-Agent": "mimalloc-pprof release preflight"}
        )
        try:
            with urllib.request.urlopen(request, timeout=15) as response:
                if response.status == 200:
                    raise ReleaseError(f"crates.io version {value['version']} already exists")
        except urllib.error.HTTPError as error:
            if error.code != 404:
                raise ReleaseError(f"crates.io version check returned HTTP {error.code}") from error
        except urllib.error.URLError as error:
            raise ReleaseError(f"crates.io version check failed: {error}") from error


def inspect_artifacts(directory: Path, value: dict[str, Any]) -> dict[str, Any]:
    actual = {
        path.name for path in directory.iterdir() if path.is_file() and path.name != "info.json"
    }
    expected = set(value["assets"])
    if actual != expected:
        raise ReleaseError(
            f"asset set mismatch: missing={sorted(expected - actual)}, extra={sorted(actual - expected)}"
        )
    artifacts: list[dict[str, str | int]] = []
    for name in value["assets"]:
        path = directory / name
        if path.is_symlink():
            raise ReleaseError(f"asset {name} is a symlink")
        size = path.stat().st_size
        if size == 0 or size > MAX_ASSET_BYTES:
            raise ReleaseError(f"asset {name} has unacceptable size {size}")
        artifacts.append(
            {"name": name, "bytes": size, "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
        )
    digest = hashlib.sha256(
        json.dumps(value, sort_keys=True, separators=(",", ":")).encode()
    ).hexdigest()
    return {**value, "directive_sha256": digest, "artifacts": artifacts}


def verify_info(directory: Path, value: dict[str, Any], info: dict[str, Any]) -> None:
    if info != inspect_artifacts(directory, value):
        raise ReleaseError("info.json identity, size, or SHA-256 differs from release artifacts")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="operation", required=True)
    for name in ("start", "resume", "status", "worker-check", "record-outcome"):
        entry = commands.add_parser(name)
        entry.add_argument("--issue", type=int, default=444)
        entry.add_argument("--candidate-sha")
        if name in ("start", "resume"):
            entry.add_argument(
                "--dry", action="store_true", help="print a plan without issue writes or dispatch"
            )
        if name == "record-outcome":
            entry.add_argument("--run-url", required=True)
            entry.add_argument(
                "--state", choices=("dry-passed", "blocked", "real-passed"), required=True
            )
            entry.add_argument("--results", required=True)
    preflight = commands.add_parser("preflight-artifacts")
    preflight.add_argument("--issue", type=int, default=444)
    preflight.add_argument("--candidate-sha", required=True)
    preflight.add_argument("--dist", type=Path, required=True)
    args = parser.parse_args()
    try:
        if args.operation == "status":
            for body in issue_comments(args.issue):
                if parse_directive(body):
                    print(body.split("State: ", 1)[-1])
            return 0
        candidate = args.candidate_sha or command("git", "rev-parse", "HEAD")
        value = directive(args.issue, source_version(), candidate)
        if args.operation == "preflight-artifacts":
            existing = issue_directives(args.issue)
            if not existing:
                raise ReleaseError("issue has no release directive")
            for record in existing:
                require_same_directive(record, value)
            info = inspect_artifacts(args.dist, value)
            (args.dist / "info.json").write_text(json.dumps(info, indent=2, sort_keys=True) + "\n")
            verify_info(args.dist, value, info)
            print(json.dumps(info, indent=2))
            return 0
        existing = issue_directives(args.issue)
        if args.operation == "start" and existing:
            raise ReleaseError("attempt already exists; use resume")
        if args.operation in ("resume", "worker-check", "record-outcome"):
            if not existing:
                raise ReleaseError("attempt issue has no directive; use start")
            for record in existing:
                require_same_directive(record, value)
        if args.operation == "record-outcome":
            state = f"{args.state}; jobs: {args.results}"
            command(
                "gh",
                "issue",
                "comment",
                str(args.issue),
                "-R",
                REPO,
                "--body",
                comment_body(value, state, args.run_url),
            )
            print(state)
            return 0
        validate_candidate(value)
        if args.operation == "worker-check":
            print(f"validated release issue #{args.issue} at {candidate}")
            return 0
        if args.dry:
            print(
                json.dumps(
                    {"action": args.operation, "dispatch": False, "directive": value}, indent=2
                )
            )
            return 0
        command(
            "gh",
            "issue",
            "comment",
            str(args.issue),
            "-R",
            REPO,
            "--body",
            comment_body(value, "dry-worker-dispatching"),
        )
        command(
            "gh",
            "workflow",
            "run",
            "auto-release.yml",
            "-R",
            REPO,
            "--ref",
            "main",
            "-F",
            "dry_run=true",
            "-f",
            f"issue_number={args.issue}",
            "-f",
            f"candidate_sha={candidate}",
        )
        print(
            f"dispatched non-publishing release worker for {candidate}; issue https://github.com/{REPO}/issues/{args.issue}"
        )
        return 0
    except (ReleaseError, OSError, json.JSONDecodeError) as error:
        print(f"release: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
