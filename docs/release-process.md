# Release attempt control (#444)

The ordinary PR and main path is fractional. Use the `ci-full` label for the
complete platform matrix. A release needs successful full dispatch runs for
every required cell in `ci/release_full_ci_manifest.v1.json`, all at the exact
merged candidate SHA. Native hosted macOS Intel and Apple Silicon are allowed
for this opt-in full/release validation.

Before starting a candidate, update the control issue body with exactly one
`- Version-bump PR: #<number>` line and one
`- Candidate merge SHA: **<full lowercase SHA>**` line. The front door verifies
that GitHub records the PR as merged to `main` at that SHA and that the resulting
commit changes the package version from its first parent. Squash and ordinary
merge results are both accepted; an absent or conflicting record fails closed.

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
- The remaining release state machine must preserve each archive's validated
  identity and hashes across retries, and verify final registry destinations.

The archive inspection gate now checks the C ZIP against candidate vendor files,
checks binary archive `PROVENANCE.txt` commit/target fields, and checks the
installed Mach-O or PE library header. It records the validated target, commit,
member list, and binary hash in `info.json`. This does **not** establish that the
full test jobs executed the same library bytes: `auto-release.yml` builds its
installable assets in separate jobs from `macos-bundles.yml` and
`windows-bundles.yml`, whose test bundles are built again.

The dry-run worker also downloads its preflight artifact in four native
`smoke-shipped-assets` jobs (hosted Intel Mac, Apple Silicon Mac, and two Windows
DLL lanes). Each job checks the exact candidate checkout, compares the outer
archive and the packaged library's SHA-256 with `info.json`, then calls
`mi_malloc` and `mi_free` through that packaged library. This is an allocation
smoke, not the full suite, and the real publication path remains fail-closed.
The control issue records `dry-passed` only after all four native smoke rows
succeed; its outcome includes the matrix result.
The allocation smoke follows the dry-run preflight upload. The stronger
release-local C suite is already a predecessor of every publication step.

The release worker now builds its C test executables in the same tree as the
installed product libraries, with `MI_DHAT=OFF` retained. A release-local native
matrix compares the installed shared and static library bytes with the test
bundle's copies, then runs the bundle's C suite on hosted macOS Intel/ARM and
Windows GNU/MSVC. This matrix is a predecessor of `release`, so a failed or
missing native row prevents every publication step. The older full CI bundles
still exercise their own configurations, including DHAT, and are not claimed
to have byte-identical libraries. The real publisher stays behind the explicit
failing fleet gate until the remaining release state machine is implemented.
