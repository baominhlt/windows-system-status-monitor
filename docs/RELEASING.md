# Releasing

Release files are never committed (`dist/` and `build/` are git-ignored); they are attached to a GitHub Release.
There are two ways to produce that release: automatically (recommended) or by hand.

## 1. Prepare the version

1. Set the new version in `VERSION` (for example `1.1.0`, no leading `v`).
2. Add a `## 1.1.0` section to `CHANGELOG.md`. Its text becomes the release description.
3. Commit these changes.

## 2a. Automatic: push a tag (recommended)

The workflow `.github/workflows/release.yml` runs when a tag starting with `v` is pushed. On a Windows runner it
checks that the tag equals `v` + `VERSION`, runs `scripts\package-release.ps1`, takes the release notes from the
matching `CHANGELOG.md` section, and publishes the Release with the zip and `SHA256SUMS.txt` attached.

Set your own identity first if this clone has none:

```bash
git config user.name  "<your name>"
git config user.email "<your public / noreply e-mail>"
```

Then tag and push:

```bash
git tag v<VERSION>
git push origin main --tags
```

Watch the run under the repository's **Actions** tab. If it fails (for example the tag does not match `VERSION`),
fix the problem, delete the tag locally and on the remote (`git tag -d vX.Y.Z`, `git push origin :refs/tags/vX.Y.Z`)
and tag again.

The repository must allow workflows to write to contents: the workflow requests `contents: write` itself, which
works with the default settings; if your organisation restricts the default token, enable *Settings → Actions →
General → Workflow permissions → Read and write permissions*.

## 2b. Optional: test the packaging locally before tagging

Pushing a tag publishes automatically, so check the assets on your own machine first. Requires the MSVC C++ x64
toolchain (see *Build from source* in the README).

```powershell
powershell -ExecutionPolicy Bypass -File scripts\package-release.ps1
```

This builds the executable and creates, in `dist\`:

- `WindowsSystemStatusMonitor-v<VERSION>-win-x64.zip`: the executable, `README.md`, `README.vi.md` and `LICENSE`
  inside one top-level folder;
- `SHA256SUMS.txt`: the SHA-256 of the zip.

Check the result: extract the zip into an empty folder, run the executable once, and compare `Get-FileHash` of
the zip with `SHA256SUMS.txt`. These local files are the same ones the workflow would publish.

## 3. SBOM (automatic)

When the **Release** workflow succeeds, the **SBOM** workflow (`.github/workflows/sbom.yml`) starts on its own. It
downloads the released zip, runs `scripts\generate-sbom.ps1` on the executable inside it, and uploads two files to the
same Release (usually a minute or two after the zip appears):

- `WindowsSystemStatusMonitor-v<VERSION>-win-x64.cdx.json` — CycloneDX 1.5
- `WindowsSystemStatusMonitor-v<VERSION>-win-x64.spdx.json` — SPDX 2.3

The SBOMs are read from the shipped binary (imported system DLLs, linker version, static C runtime, optional
run-time components), so they describe exactly what was released. Notes:

- **Re-create or backfill:** *Actions → SBOM → Run workflow*, give the tag (for example `v1.0.0`). Existing SBOM
  assets of that release are replaced.
- `workflow_run` workflows only fire when `sbom.yml` exists on the default branch, so merge it before tagging.
- The SBOM workflow takes the tag from the triggering run. If it ever reports that the branch name is not a tag,
  start it by hand with the tag instead.
- Locally (needs a built exe and a zip): `powershell -ExecutionPolicy Bypass -File scripts\generate-sbom.ps1 -Exe build\WindowsSystemStatusMonitor.exe -Zip dist\<zip> -Version <VERSION>`
  writes both files to `dist\`.
- What an SBOM from a binary cannot know: versions of Windows system DLLs (they depend on the Windows build), and the
  exact compiler build (only the `major.minor` linker version is recorded).

## Publishing by hand instead

The tag push in step 2a is what triggers the workflow, so do not push a tag if you want to publish manually.
Instead delete `.github/workflows/release.yml` (or disable the workflow under the **Actions** tab), then on GitHub
choose **Releases → Draft a new release**, create the tag `v<VERSION>` there, paste the matching `CHANGELOG.md`
section as the description, and upload **both** files from `dist\`.

The executable is not code-signed; the automatic release notes already say so, and you should mention it in a
hand-written one too.
