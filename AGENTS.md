# Notes for coding agents

Read CONTRIBUTING.md first; its rules apply to you.

- Firmware and hardware changes go in separate branches and pull requests.
- Build firmware with `sw/scripts/build_release.sh` (Pico SDK 2.3.1). Do not
  hand-roll cmake invocations for release files.
- `pgusinit/pgusinit.c` and `pgusinit/README.md` use CRLF line endings. Keep them.
- Release file names must fit DOS 8.3.
- Experimental features must merge cleanly on top of `main` and alongside each
  other. Check with `sw/scripts/experimental.sh merge --worktree <dir>`.
- Nothing is verified until it has run on a card. Say "builds" or "passes host
  tests", not "works", for untested firmware.
- Do not create tags or releases, and do not delete branches, unless asked.
