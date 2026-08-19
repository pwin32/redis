# Windows release and CI policy

This policy is part of the public Redis Windows port. It describes what the
repository workflows enforce and what still requires a repository administrator
to configure in GitHub settings.

## Qualification gates

- Pull requests run public-source hygiene, a MinGW64 build, focused Redis
  tests, and legacy/modern Windows interop smoke coverage.
- Pushes to `mingw-8.10`, `mingw-8.8`, `mingw-7.4`, `mingw-7.2`, and `mingw-6.2`
  run the full Windows qualification. The gate includes the root, Cluster,
  Sentinel, and Module API suites; interop; the elevated service/Event Log
  test; package extraction, license, PE/import and resource checks; packaged
  replication; and a 30-minute QFork persistence soak.
- A release dispatch takes an exact 40-character tip SHA. Before Windows
  qualification is called for that SHA, the workflow verifies that every one
  of the five maintained branch tips has a successful full-qualification run.
  No release is published while one line is stale or failing.

## Release scope and order

After upstream publishes a maintenance release, update and fully qualify all
five lines in this order: 8.10, 8.8, 7.4, 7.2, and 6.2. Only then publish the
release-eligible lines in this order: 8.10, 8.8, 7.2, and 6.2. The 7.4 line is
updated and tested but is not published under the current license policy. For
Redis 8, only the latest two minor lines are release eligible.

Upstream imports use a merge commit that retains the upstream Redis and
tporadowski ancestry, followed by a narrow replay of the upstream delta where
the historical Windows lineage diverges. Normal maintenance changes use
reviewed pull requests and squash merge. The intended repository setting is at
least one approving review, required passing PR smoke checks, and no force push
after the first public release.

## Package and evidence contract

Packages are built only in GitHub Actions. Each release carries the ZIP,
`SHA256SUMS.txt`, `BUILDINFO.txt`, `package-manifest.txt`, `toolchain.txt`,
`TEST-REPORT.md`, `release-evidence.json`, `benchmark.csv`, and
`sbom.spdx.json`. The benchmark uses loopback, persistence disabled, one
benchmark thread, 50 clients, pipeline 1, a discarded 10,000-request warmup,
and three 50,000-request measurements for PING, SET, and GET. A throughput
decrease or p95 increase above 15% is an advisory warning; it does not silently
turn into a release blocker.

The release workflow creates GitHub artifact attestations for both SLSA build
provenance and the SPDX SBOM. It creates the remote
`vX.Y.Z-windows.N` tag only after those attestations and all qualification
gates succeed. Publication creates all assets in a single release operation
and refuses to modify an existing release or move an existing tag. Repository
administrators must also enable GitHub's release immutability setting before
the first public release so later UI or API operations cannot alter the tag or
assets.

## Credentials and settings

The workflows use the run-scoped `GITHUB_TOKEN` with job-level permissions.
They do not require a personal access token. A personal token pasted into a
chat, log, or issue must be revoked and replaced through a secure channel.
Repository rulesets, required-review settings, Actions approval policies, and
release immutability are administrative settings and are not encoded as
secrets in this repository.

Before the first public release, a repository administrator must:

- enable immutable releases;
- allow the pinned GitHub-owned actions and `msys2/setup-msys2` used by these
  workflows;
- protect every canonical `mingw-*` branch against deletion and, after the
  first public release, force pushes;
- require pull requests, one approving review, resolved conversations, and the
  `public-hygiene` and `mingw-smoke` PR checks;
- keep both squash merging and merge commits available: ordinary maintenance
  is squash-merged, while upstream imports use merge commits to preserve
  Redis and tporadowski ancestry; and
- leave Dependabot auto-merge disabled. Dependabot is a throttled proposal and
  security-notification channel, not evidence that a change is qualified.

The release workflow itself rechecks the successful full-qualification run and
current tip of all five maintained lines immediately before it creates a tag.
It refuses to overwrite an existing release or move an existing tag.
