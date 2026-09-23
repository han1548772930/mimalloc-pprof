# Release attempt control (#444)

The ordinary PR and main path is fractional. Use the `ci-full` label for the
complete platform matrix. A release needs successful full dispatch runs for
every required cell in `ci/release_full_ci_manifest.v1.json`, all at the exact
merged candidate SHA. Native hosted macOS Intel and Apple Silicon are allowed
for this opt-in full/release validation.

`ci/release.py` is the attempt front door. Issue #444 is the live control record
for the v1.0.1 pilot. After a reviewed version bump is merged to `main`, use a
clean checkout of that main commit:

```sh
python3 ci/release.py start --issue 444 --candidate-sha <40-character-SHA> --dry
python3 ci/release.py start --issue 444 --candidate-sha <40-character-SHA>
python3 ci/release.py status --issue 444
python3 ci/release.py resume --issue 444 --candidate-sha <same-SHA>
```

`--dry` on the front door only prints the plan. Without it, `start` or `resume`
appends the versioned directive to the issue and dispatches a **non-publishing**
`auto-release.yml` worker. That worker checks issue identity and the exact main
SHA before any build, builds the five declared archives, runs Cargo publication
dry-run, and uploads a preflight artifact containing `info.json` with sizes and
SHA-256 hashes. Its `dry_run=true` path creates no Git tag or GitHub Release.
Tag pushes cannot trigger this workflow.

The current real publication job remains disabled by an explicit failing gate.
Before opening that gate, implement and test the retryable destination state
machine: verify the full run IDs against every manifest cell; preserve the same
issue, SHA, tag, and hashes on resume; check crates.io package size and registry
state; publish the crate and a complete draft GitHub Release from one worker;
retry transient GitHub failures up to ten times; finalize only after both
destinations are verified; verify that `v1.0.1` resolves to the candidate SHA.
Crates.io and GitHub cannot be one atomic transaction, so a partial result must
stay recorded on the issue and resume without changing identity.

Two additional gates must be closed before the real publisher is enabled:

- The front door currently proves that the candidate is an ancestor of `main`
  and has the requested version. It must also prove that this SHA is the
  intended, reviewed version-bump merge commit recorded by the release issue;
  an arbitrary older main commit with the same version is insufficient.
- `info.json` currently proves the five outer archive names, sizes, and hashes.
  The release preflight must inspect each archive's internal target identity and
  provenance, then prove that the shipped bytes are the same bytes exercised by
  the exact-SHA full test bundles. A matching filename alone is insufficient.
