Tekken: Dark Resurrection Wii - install

Copy the contents of this folder to:

  sd:/apps/tkdr/       (or usb:/apps/tkdr/)

Required:

  boot.dol             the DOL built from this repository
  image.bin            generated from your decrypted ULUS10139 EBOOT
  tekken_dr.bin        your own ISO dump (or tekken_dr.iso)

The launcher also accepts the image under any of these names:
  tekken_dr.iso, tekken_dr.bin, Tekken DR.iso, ULUS10139.iso,
  a-tekken.iso, game.iso

The .bin spelling is useful when a Wii file manager tries to mount every
.iso it finds and chokes on a PSP image.  The bytes are still an ISO9660
image; only the filename differs.

CSO is not read at runtime; convert it back to ISO first for this port.

Saves are created under ms0/ on the SD card.
See README.md in the source repository for the full build procedure.
No game data is distributed with this project.

This is an unofficial, non-commercial fan project. Tekken: Dark Resurrection
is a trademark of Bandai Namco Entertainment Inc. It is not affiliated with or
endorsed by Bandai Namco or Sony.

Licences and credits: see LICENSE, THIRD_PARTY.md and CREDITS.md in the
source repository. Do not redistribute the ISO or a built DOL.
