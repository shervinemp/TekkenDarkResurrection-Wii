# Tekken: Dark Resurrection (ULUS10139 v1.00) profile

This profile is for the dump supported by tkdr-recomp:

```text
size   1750237184
sha1   8193a65dee49685db49284408c89bdb35324bc32
crc32  3ffd7a9d
```

The ISO's `PSP_GAME/SYSDIR/EBOOT.BIN` is encrypted (`~PSP`). Generate with a
decrypted EBOOT dumped by PPSSPP:

```sh
python tools/prepare_tkdr.py --verify-only --iso path/to/a-tekken.iso
python tools/prepare_tkdr.py --iso path/to/a-tekken.iso \
                             --eboot path/to/ULUS10139_EBOOT.BIN
```

The generator uses the upstream tkdr `recomp.emit` path (sectioned PRX with
relocations), which is the most complete route to a playable Wii build.
