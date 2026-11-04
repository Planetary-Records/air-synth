# Alpha release procedure

This is an alpha, before beta. Source preparation is not publication or final qualification. Never replace an existing release's bytes. Keep a rollback copy and write the page/download rollback before launch.

1. Freeze the exact product/source/version and review changed code, notices, privacy and rights. Preserve compatibility IDs and legacy filenames. Run required local checks, then the final complete release gate once on the reviewed candidate.
2. Bind the shipping module, matching helper and module metadata to full hashes, build inputs and validation. Exclude icons/desktop metadata, PDBs, credentials, logs and model weights from the admitted three-file bundle.
3. Supply the exact corresponding project/dependency source, original notices and guarded build patches. Rebuild the selected source independently. A dependency pin alone is insufficient.
4. Package with tools/release_package.py using a new output and an accepted record containing source_commit, documentation_commit, documentation_repository and complete file hashes/sizes. The documentation repository/commit must be the actual public snapshot so unbundled links work. The legacy private default is not a public-source destination.
5. Verify every ZIP member and checksum, then stage/compile the installer through tools/release_installer.py. Qualify the matching installer, upgrade, user-file preservation, uninstall and warnings on a protected clean machine. Keep host/listening/new-user acceptance distinct.
6. Publish only after the exact cleared tree and launch actions receive written owner sign-off. Use a new prerelease, not Latest. Match page links/version/hash to those assets, read back a fresh download and keep feedback available before announcements.

No script here authorizes public visibility, paid execution, messages or replacement of a musician's installation. See [licensing](licensing.md), [dependency source](dependencies.md), [source/artwork scope](provenance-review.md) and [installer development](../development/installer.md).

Current candidate: `0.1.0-alpha.2`. Attach its new executable installer to the matching GitHub prerelease; the landing page must use that exact `.exe` asset URL and checksum. Check the manual and feedback links in the live readback. Public publication requires the reviewed destination and source tree; private development history is excluded.
