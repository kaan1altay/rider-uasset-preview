# Preparing a GitHub Release

The source repository contains everything needed to prepare distribution. It does not automatically publish releases or upload binaries.

## Build the packages

1. Keep the Rider `plugin.xml` version and Unreal `.uplugin` `VersionName` aligned. Increment the Unreal descriptor's numeric `Version` when releasing a new version.
2. Build and test the Rider plugin with `scripts/Build-Rider.ps1 -Test`.
3. Close Unreal Editor and Live Coding, then run `scripts/Build-Unreal.ps1`. Keep the host/project path and compiled package path printed by that build.
4. Run the real Unreal integration tests described in [DEVELOPMENT.md](DEVELOPMENT.md).
5. Prepare the distribution from the compiled package:

```powershell
.\scripts\New-PluginRepository.ps1
.\scripts\Package-Release.ps1 -UnrealPackage '.\dist\RiderAssetPreview-UE5.8-<timestamp>'
.\scripts\Test-Distribution.ps1
```

Run these in PowerShell 7 from the repository root. `Test-Distribution.ps1` uses fixture profiles, a fake project and simulated application process states; it never installs into your normal Rider profile or Unreal project. Tests cover offline installs, updates/backups, rollback after a copy failure, active-application guards, archive traversal rejection, corrupted hashes, engine build matching, `-WhatIf`, and repository metadata. Network downloads and Rider's repository UI still need a check against the published release.

`Package-Release.ps1` verifies the Unreal package source and Engine build ID, checks the Rider package ID/version, and generates five distribution files. Versioned ZIP names come from plugin metadata. `New-PluginRepository.ps1` refreshes the checked-in `updatePlugins.xml`; commit this change with the version bump. If forking the project, pass `-Repository 'owner/repository'` to both scripts and update the installer's default repository and README download links.

## Review the release

For version **0.2.1**, attach all five files from `dist/` to a GitHub Release tagged **`v0.2.1`**:

- `unreal-asset-preview-0.2.1.zip`
- `RiderAssetPreview-UE5.8-0.2.1.zip`
- `release-manifest.json`
- `updatePlugins.xml`
- `Install.ps1`

Create a draft in the repository's **Releases → Draft a new release** page, choose the matching source commit/tag, and upload the files. State the supported Rider version, Unreal version/build and current limitations in the release notes. Review the draft before publishing. Do not upload `build/`, session descriptors, engine assets, Rider libraries or PDBs.

The release manifest contains exact Unreal version/build compatibility and the size/SHA-256 of each ZIP. Hashes detect incomplete or mismatched downloads; they are not a separate code-signing mechanism.

The installer resolves `latest` once through the GitHub Releases API, then downloads everything from that fixed tag. Explicit `-Version '0.2.1'` selects `v0.2.1`. ZIP URLs inside `updatePlugins.xml` also use an immutable version tag; do not replace published release assets with unrelated builds under the same version.

## After publication

1. Confirm the five release files download successfully.
2. Use the README's one-command installation on a disposable profile/project with matching Engine build. `-RiderPluginsDirectory` can select an isolated plugin directory.
3. Add the repository URL in a Rider test profile and install/search for **Unreal Asset Preview**:

```text
https://github.com/kaan1altay/rider-uasset-preview/releases/latest/download/updatePlugins.xml
```

4. Verify a mesh and Blueprint preview against the running Unreal project.

Use stable releases for this URL: GitHub's `latest` endpoint does not select drafts or prereleases. For a prerelease, use its explicit tag URL instead. Rider repository updates affect only the Rider plugin; the matching Unreal companion must also be updated using the installer.
