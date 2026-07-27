# Legacy Visual Studio packaging quarantine

The Visual Studio solution is retained as a best-effort source/build reference,
but the MSI, NuGet, and Chocolatey publication paths are not part of the Redis
7.4.10 Windows release.

Those historical paths still assume the former signed-binary layout and carry
MSOpenTech-era package ownership, URLs, and installer license presentation.
They do not stage the Redis 7.4 `LICENSE.txt`, `REDISCONTRIBUTIONS.txt`,
`WINDOWS-NOTICES.txt`, `THIRD-PARTY-NOTICES.txt`, or GCC/MinGW runtime license
payload. Producing or publishing one of those packages as Redis 7.4 would be
incorrect.

Accordingly:

- `Build.bat` builds only the Visual Studio server, CLI, and benchmark targets;
- `setups/CreatePackages.ps1` and `setups/PushPackages.ps1` fail closed; and
- the WiX, NuGet, and Chocolatey files remain historical implementation
  material, not current release metadata.

Use the root `package-mingw.sh` workflow for the release-gated portable ZIP.
Reactivating another package format requires a new manifest derived from that
canonical payload, an updated installer license UI, complete clean-machine
validation, and an explicit release decision.
