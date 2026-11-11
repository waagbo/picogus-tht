# CD images in a CDROM folder on the USB drive

Experimental feature, branch `sw/cdrom-subdir`.

## CD-ROM

- CD images are now also found in a folder named `CDROM` at the top level of the USB drive (any letter case). `pgusinit /cdlist` shows the root images first, numbered exactly as before, followed by the folder's images as `CDROM/NAME.EXT`. Image auto-advance cycles through the whole list, so it now continues from the last root image into the `CDROM` folder before wrapping around.
- `pgusinit /cdloadname` accepts `CDROM\NAME.CUE`, `\CDROM\NAME.CUE` or `CDROM/NAME.CUE` for images in the folder. A bare `NAME.CUE` is looked for in the root first, then in the `CDROM` folder. Names are matched case-insensitively and the loaded image is reported with the name as it is on the drive.
- A `.cue` in the `CDROM` folder finds its `.bin` next to it: `FILE` references without a path are taken relative to the `.cue`'s folder; references containing a path are used as given, relative to the drive root, with `\` accepted as separator.
- Loading by name accepts exactly the names `/cdlist` shows; anything else is refused with "No file ... on USB" instead of loading an image the list does not know about.
- Without a `CDROM` folder, the list, numbering and messages are the same as in the mainline firmware.
- Image names are bounded everywhere: the firmware accepts at most 133 characters (`CDROM/` + a 127 character name) from `/cdloadname` and drops anything beyond that instead of overrunning; over-long `FILE` names in a `.cue` are rejected with an error.

## pgusinit

- Help text and README describe the `CDROM` folder. `/cdloadname` still sends at most 127 characters, so a new pgusinit can never overrun the name buffer of older firmware; an image in the `CDROM` folder whose name is longer than 121 characters can be loaded by index.
