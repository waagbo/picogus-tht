# Experimental features

Features that are not part of the mainline firmware yet are developed on their
own git branches and can be built into an *experimental* flavor of the release
files, next to the mainline ones:

| | Mainline | Experimental |
|---|---|---|
| Source | the branch being built | the same commit with every feature branch merged in |
| Files | `binaries/` | `binaries/experimental/` |
| Firmware version | `v4.2.0` | `v4.2.0+cdrom-subdir+pgdfs+webserver` (lists the features) |

Both flavors contain the same set of files: the firmware images for RP2040,
RP2350 and PicoGUS v2.1, and the DOS programs. Experimental features may
change `pgusinit.exe` and the control protocol and may add DOS programs and
firmware images of their own, so use the files of one flavor together.

The features at present:

| Feature | Branch | What it adds |
|---|---|---|
| `cdrom-subdir` | `sw/cdrom-subdir` | CD images in a `CDROM` folder on the USB drive |
| `pgdfs` | `sw/pgdfs` | the USB drive as a DOS and Windows drive letter; `PGDFS.EXE`, `DFSDIAG.EXE`, `win9x/PGDFS.VXD`, `winnt/pgdfs.sys` |
| `webserver` | `sw/webserver` | a web page on Pico W and Pico 2 W boards; firmware images `picogusw.uf2` and `pg2350w.uf2` |

Each has its notes in `sw/experimental/<feature>.md` on its branch, and in
`EXPERIMENTAL.md` of the experimental set.

## The feature branches

[`branches.txt`](branches.txt) lists the branches that make up the experimental
flavor. To add a feature, add its branch there; to drop one, remove its line.

A feature branch

- is based on (and regularly merged with) the mainline branch, so that it
  merges cleanly on top of it and alongside the other features. Features
  often add to the same places (a mode's main loop, the control port
  handlers, a README section); where two of them add lines at the very same
  spot git can't combine them, so one of the two moves its lines a little;
- describes itself in `sw/experimental/<feature>.md`, a file only that branch
  has. Its first line is a `# Title`, the rest is the feature's change notes.
  This file is what marks a source tree as carrying the feature: the build
  adds `+<feature>` to the firmware version for every such file, also when the
  feature branch is built on its own;
- keeps its notes in that file rather than in `sw/CHANGELOG.md`, where every
  feature would otherwise edit the same lines. The notes move to the changelog
  when the feature becomes part of the mainline.

## Building

```sh
sw/scripts/build_release.sh                  # mainline: binaries/
sw/scripts/build_release.sh --experimental   # mainline and binaries/experimental/
sw/scripts/build_release.sh --only-experimental
```

The experimental set is built from a separate checkout,
`build/experimental-src`, that `sw/scripts/experimental.sh` creates from the
current commit and merges the feature branches into, so the tree you are
working in is not touched. A local branch is used when there is one of that
name, otherwise the one on the remote (`--remote`, `--fetch`). If a branch
doesn't merge cleanly the build stops and names the conflicting files: bring
that feature branch up to date with the mainline branch and resolve it there.

```sh
sw/scripts/experimental.sh list
sw/scripts/experimental.sh merge --worktree /tmp/picogus-exp   # just the merged source
sw/scripts/experimental.sh describe                            # features in this tree
```

CI builds both flavors for every push and attaches both to releases; see
`.github/workflows/build.yml`. A problem in the experimental flavor shows up
as a failed job there, but doesn't hold back the mainline files or a release.
The experimental flavor can be left out of a manual run, or of every run with
the repository variable `PICOGUS_EXPERIMENTAL` set to `false`.
