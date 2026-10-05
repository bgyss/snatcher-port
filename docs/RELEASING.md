# Releasing

Releases are GitHub Releases built by `.github/workflows/release.yml` from a version tag. Every release contains interpreter-only player builds with no game data (players supply their own disc), so nothing disc-derived is ever published.

## Cut a release

```sh
git switch main && git pull
tools/release.sh v0.2.0          # or v0.2.0-rc1 for a pre-release (any -suffix)
```

`release.sh` checks the tag format (`vMAJOR.MINOR.PATCH[-suffix]`), a clean tree, that you are on `main` and level with `origin/main`, and that the tag is new; then it creates an annotated tag and pushes it. You can also tag by hand (`git tag -a v0.2.0 -m ... && git push origin v0.2.0`) as long as the name matches `v[0-9]+.[0-9]+.[0-9]+*`.

The tag triggers the `release` workflow:

1. It reuses `build.yml` to build and test all three platforms (macOS universal, Windows x64, Linux x86_64).
2. If all three pass, it creates the release with generated notes, a pre-release flag for suffixed tags, and these assets:
   `Snatcher-<tag>-mac.zip`, `Snatcher-<tag>-windows-x64.zip`, `Snatcher-<tag>-linux-x86_64.tar.gz`, `SHA256SUMS.txt`.

If a build fails nothing is published; fix it, delete the tag (`git push origin :refs/tags/<tag>`, `git tag -d <tag>`) and tag again.

## What CI builds

`tools/package/ci_package.sh mac|windows|linux` (used by `build.yml` on every push and PR too):

| | macOS | Windows | Linux |
|---|---|---|---|
| Runner | macos-latest | windows-latest + MSYS2 MinGW-w64 | ubuntu-22.04 |
| Output | universal arm64 + x86_64 app, macOS 12+ | single static `snatcher.exe` | `snatcher`, glibc 2.35+, static libstdc++ |
| SDL3 | fetched, static | fetched, static | fetched, static (X11/Wayland/audio loaded at runtime) |

Every build is interpreter-only, runs the disc-free tests (`replay_test`, `watchdog_test`, `tools/playtest/test_playtest.py`; its disc tests skip) and fails if the binary contains translated code. The macOS and Windows builds are unsigned (Gatekeeper / SmartScreen warn on first launch; the bundled READMEs explain how to open them).

## Notes

- Workflow changes to `release.yml` can only be dry-run (Actions > release > Run workflow, which builds without publishing) once the file is on `main`. The very first publish is best done with a pre-release tag such as `v0.1.0-rc1`.
- `tools/test_release_sh.sh` checks that `release.sh` refuses malformed tags, wrong branch, and unknown options (it never tags).
