# Experimental native CHD support

This branch is experimental and has not been validated on a physical PS2.

## Current scope

- CHD v5 DVD images with 2048-byte logical sectors.
- Only zlib codec slot 0, uncompressed hunks and self-hunk aliases are supported.
- Parent images, CD-track CHDs and other codecs (LZMA, Huffman, FLAC, Zstandard) are rejected.
- Hunk size must be 4 KiB–256 KiB; images with more than 40,000 hunks are rejected to limit memory use.
- CHD data/map offsets must fit in 32 bits in this first prototype.

## Create a compatible CHD

Use MAME's chdman with zlib only and 256 KiB hunks:

```powershell
chdman createdvd -i "Game.iso" -o "Game.chd" -c zlib -hs 262144 -us 2048
```

Default compression choices may include unsupported codecs. Successful conversion does not guarantee game compatibility.

## Validation

The host-test workflow generates a DVD ISO, converts it to a zlib-only CHD, compiles this reader and compares every decoded sector against the ISO. The regular OPL build workflow also runs on pushes.

A passing host test and ELF build do not prove gameplay on a physical PS2. Keep this branch isolated until tested on hardware.

## Third-party code

Huffman and bitstream helpers are derived from libchdr (BSD-3-Clause). The bundled miniz inflater is distributed under the Unlicense/public-domain dedication; source notices are retained.
