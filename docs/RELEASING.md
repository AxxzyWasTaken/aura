# Releasing Aura

Aura has its own version series, starting at `v0.1.0`. Keep feature work in the internal repository separate from the public release branch.

1. Update `docs/releases/<version>.md` with changes, known limits, and evidence for any hardware qualification.
2. Commit coherent changes to public `main`. Require green build, format-check, test, and release artifact jobs for that exact source revision.
3. Create an annotated `v<version>` tag on that revision and push it to `aurora-silicon/aura`.
4. The release workflow builds both stages, packages binaries and matching debug ELF files with licenses and source/build manifests, checks SHA-256 sums, and creates a **draft** GitHub release.
5. Review/edit the draft notes and qualify the exact artifacts on the intended hardware before publishing. The workflow does not publish drafts and refuses to alter an already published release.

Main-branch builds produce Actions artifacts but do not create GitHub releases. A manual dispatch also builds artifacts without publishing or creating a release. Re-running a tag build may replace assets on its existing draft; it preserves edited release notes.

Local packaging uses `tools/package_release.py` after a clean-source `RELEASE=1 CHAINLOADING=1` build, with `ESP_STAGE2=0` for `stage1` or `ESP_STAGE2=1` for `stage2`. Set `AURA_VERSION_TAG` to the release tag and pass the same value to `--version`. Use `--compiler` to record the actual compiler when cross-compiling.

The release archives contain Aura components. A complete UEFI boot bundle additionally needs board-matching DTBs and a matching U-Boot payload; those versions and their hashes must be recorded separately. No deployment or firmware installation is performed by release CI.
