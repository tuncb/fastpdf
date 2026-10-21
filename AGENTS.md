# Repository instructions

## Development and verification

- Use `D:\.worktree` for Git worktrees on Windows.
- Target Windows x64 only. Use C++20, structs, and free functions.
- Build and test Release with `./scripts/build.ps1 -Configuration Release`. Use Visual Studio, the Windows SDK, CMake, Ninja, and Python 3.10 or newer.
- Run all CTest checks, including Python integration and codec tests, before release. Resolve build warnings and test failures.
- For release tooling changes, run `python -m unittest discover -s .github/scripts -p 'test_*.py'` and `node --test .github/scripts/resolve-release-build.test.cjs`.
- Keep generated builds, downloaded reference data, and release artifacts in the ignored build or artifacts directories.
- Installation and update tests must use isolated filesystem and registry fixtures; never modify a real user installation as part of CTest. Updates are manual-only, with no tray icon or scheduled/background checks.

## Version updates

- Use semantic versions `MAJOR.MINOR.PATCH` and matching Git tags `vMAJOR.MINOR.PATCH`. Increment PATCH for fixes, MINOR for features, and MAJOR for incompatible stable releases. Before 1.0, use a MINOR increment for incompatible changes.
- The canonical version is `project(FastPDF VERSION ...)` in `CMakeLists.txt`.
- Keep `src/app.rc` in sync: `FILEVERSION` and `PRODUCTVERSION` use `MAJOR,MINOR,PATCH,0`; the `FileVersion` string uses `MAJOR.MINOR.PATCH`.
- Keep the FastPDF assembly identity in `src/app.manifest` at `MAJOR.MINOR.PATCH.0` with `processorArchitecture="amd64"`.
- Update `CHANGELOG.md` before committing. Use the matching `## vX.Y.Z` heading, newest first. Review the commits and actual changes since the last release. Include notable user-visible changes; omit agent instructions, experiments, and minor internal cleanup.
- Validate with `python .github/scripts/release.py validate-tag vX.Y.Z`.

## Release process

1. Work on `main` with no unrelated uncommitted changes. Update all version fields and the changelog.
2. Run the release tooling tests and `./scripts/build.ps1 -Configuration Release`. Then run `./scripts/package.ps1` and `python .github/scripts/release.py verify artifacts/release`.
3. Commit the complete release changes and wait for the commit to succeed. Push `main` with `git push origin main`.
4. Only after the commit succeeds, create and push the matching tag. On this workstation, the Woby convention is `nu C:\tools\gittag.nu vX.Y.Z`; this creates the tag and pushes the branch and all local tags. Use it only if all local tags are intended for publication. The portable equivalent is `git tag vX.Y.Z` followed by `git push origin vX.Y.Z`.
5. Check both the **Build and Test** and **Release** workflow runs. Confirm the GitHub release has only `fastpdf.exe` and `SHA256SUMS.txt` as uploaded assets.

The workflows follow Woby's build/release split. Pushes to `main` build and test Windows x64, then upload the portable package. A pushed `vX.Y.Z` tag validates the version and changelog and must point to a commit reachable from `origin/main`. Branch pushes alone do not publish releases.

The release workflow waits for and reuses a successful `main` build of the exact tagged commit. If no usable build exists or its artifacts expired, it invokes the same build workflow on the tag. It verifies the executable checksum, embedded commit/version manifest, embedded data licenses, release file set, and x64 executable architecture before publishing. Release notes come from the matching changelog entry.

The standalone `fastpdf.exe` embeds data licenses and `release-manifest.json` as Windows resources. The notices are accessible from the Help menu. The Microsoft C/C++ runtime is statically linked. `pdfprobe.exe` is built for development and integration tests but is not installed or published. Only Windows x64 is built and released.

If a workflow fails, fix the cause and rerun the failed run when appropriate. You can also dispatch **Release** with an existing tag selected. Never move or overwrite a published version tag; ship fixes as a new version. Rerunning a release uploads verified assets to the existing release.
