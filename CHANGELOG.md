# Changelog

## 0.2.1 — Rider compatibility (2026-11-07)

- Extended declared Rider compatibility through build 262 (2026.2).
- Removed the installer's hard-coded 2025.3 restriction; validated manifest bounds must also match the packaged plugin descriptor.
- Added installer regression coverage for 2026.x, future/old build rejection and inconsistent compatibility metadata.
- Added a repeatable offline JetBrains Plugin Verifier command for the exact distribution ZIP.
- Updated build tooling for the 2026.2 VCS JAR location, Windows command-length limits and JBR 25 test-fixture cleanup.
- Retained the version 2 Unreal inspection protocol; the 0.2.0 Unreal companion remains usable with the 0.2.1 Rider hotfix.

## 0.2.0 — Asset review (2026-11-07)

- Added native two-way `.uasset` review in Rider's Commit/Git History diff viewers, including added and deleted assets.
- Added isolated historical packages, linked before/after cameras, wipe and pixel-difference modes.
- Added stable component matching, structural/settings differences, searchable ownership labels and copied review summaries.
- Added Blueprint node settings/defaults/connection inspection, excluding layout and comments.
- Added setting explanations, units for common properties, Unreal tooltip fallback and property search in ordinary asset tabs.
- Added settings inspection for non-Actor Blueprints and generic asset classes, plus derived review facts for meshes, textures and Blueprint inheritance.
- Added verified local Git LFS revision resolution and worktree support. Missing revisions are never replaced with working-copy data.
- Added bounded snapshots, partial-coverage notices, client-known cleanup IDs and scoped diff-package retention cleanup.
- Added regression fixtures and an isolated real-Git review demo.

## 0.1.0 — Initial preview

- Read-only asset previews for Actor Blueprints, Static Meshes, Skeletal Meshes and Texture2D.
- Clickable component hierarchy, Unreal Details data, camera controls and Advanced fields.
- Companion Unreal editor plugin, authenticated loopback bridge, installer and GitHub Release distribution.
