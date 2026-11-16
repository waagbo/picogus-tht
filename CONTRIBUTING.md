# Contributing

PicoGUS THT is a fork of [polpo/picogus](https://github.com/polpo/picogus) that adds
through-hole hardware and firmware features of its own. These rules keep the three
kinds of work in this repository apart.

## Branches

- `main` is the only long-lived branch. It must always build, and changes reach it
  through pull requests.
- Use short-lived topic branches and delete them after merging:
  - `sw/<feature>` for firmware and DOS tools
  - `hw/<board>` for hardware
  - `sync/upstream-<version>` for merging an upstream release

## Firmware

- Keep changes to upstream files small, and put new code in separate files where
  possible, so upstream releases stay easy to merge.
- One source tree serves both the RP2040 and the RP2350. Chip differences belong in
  `sw/system/platform.h` and the board headers.
- Experimental features live on their own `sw/<feature>` branch, listed in
  `sw/experimental/branches.txt`, and describe themselves in
  `sw/experimental/<feature>.md`. Keep the branch merged with `main`. A feature moves
  into `main` once it has been tested on a card.
- See `sw/experimental/README.md` and `sw/docs/rp2350.md` for building.

## Hardware

- Hardware branches change only the `hw-*` folders. Firmware for a new board goes in
  a separate firmware pull request.
- Each board has its own folder with its documentation and ordering files.

## Testing

- CI must pass before merging.
- Firmware changes that affect a card's behaviour need a test on real hardware
  before they are released. Record what was tested in the pull request.

## Releases

- Firmware: tag `v<upstream version>-tht.<n>`, for example `v4.2.0-tht.1`. CI builds
  the release files for every platform.
- Hardware: tag `hw-<board>-v<revision>`, for example `hw-tht-v1.3.2`.
