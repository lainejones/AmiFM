# Icon tooling (host-side)

These are **host** Python 3 scripts that generate / inspect the Amiga colour
icon `AmiFM.info`. They are not part of the Amiga program and are not compiled
into it — they just produce the binary `.info` asset.

| Script | What it does |
|--------|--------------|
| `mkglow.py` | Designs the 64×44 glossy dual-pane window icon and serialises a real OS 3.5/3.2 **colour icon**: classic `DiskObject` + planar fallback + appended `FORM ICON` (FACE + two RLE `IMAG` chunks — normal + gold glow-on-select). Writes `../AmiFM.info`. **This is the one `make icon` runs.** |
| `decode_glow.py` | Decodes a real GlowIcon's RLE image chunks — used to reverse-engineer the format (bit-level PackBits). Reference / debugging only. |
| `verify_glow.py` | Round-trip-decodes `AmiFM.info`'s own image chunks back to a preview grid, to confirm the encoder is correct. |
| `mkicon.py` | The earlier **planar-only** icon generator (2/3 bitplanes, no glow). Superseded by `mkglow.py`; kept for reference. |

## Colour-icon format notes (learned the hard way)

- Colour icons **must** use RLE (`imgfmt`/`palfmt` = 1) bit-level PackBits;
  uncompressed `IMAG` chunks are rejected by `icon.library`.
- Control byte = 8 bits MSB-first: `n<128` → copy `n+1` units; `n>128` → repeat
  `257−n`; `n==128` → no-op. Units = depth bits (image) or 8 bits (palette).
- The `FORM ICON` IFF is appended **after** the classic `DiskObject` + planar
  image, so old systems see the planar icon and new ones see the colour one.
- The glow = a solid bright-gold edge hugging the silhouette + a Bayer
  ordered-dither gold fade outward (icons have no alpha, so the dither fakes the
  soft falloff).

Preview pipeline: the scripts dump `*_grid.txt`, which a host PowerShell snippet
renders to PNG on Workbench-grey for eyeballing.
