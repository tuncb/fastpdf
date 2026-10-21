# Version update workflow

- When the user asks for a version update, update the canonical application version - The canonical version is `project(FastPDF VERSION ...)` in `CMakeLists.txt`.
- Keep `src/app.rc` in sync: `FILEVERSION` and `PRODUCTVERSION` use `MAJOR,MINOR,PATCH,0`; the `FileVersion` string uses `MAJOR.MINOR.PATCH`.
- Keep the FastPDF assembly identity in `src/app.manifest` at `MAJOR.MINOR.PATCH.0` with `processorArchitecture="amd64"`.
- Update `CHANGELOG.md` before committing the version update. Use the matching tag (`vX.Y.Z`) as the version heading and keep the newest version first.
- Review commit messages and actual changes since the previous release tag. Add brief entries for notable changes. Omit minor fixes, agent instruction files, experiments, and investigations.
- If there are no unrelated changes in the repository, commit the version update, including `CHANGELOG.md`, and wait for the commit to finish. Only after the commit succeeds, create the matching tag with `nu c:\tools\gittag.nu vX.Y.Z`.

# Code changes
- Build the project in release mode, the build should succeed without any warnings.
- If there is already an instance open, terminate the process and re-try.

# Code style rules

- Use procedural programming style. Structs + free functions.
- No inheritance, no private or protected members.

# UI rules

- All ui items should obey theme color schemes.

# Writing Style

Use the ASD-STE100 standard.