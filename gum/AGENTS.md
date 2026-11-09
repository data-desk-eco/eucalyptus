# gum

A C port of `notebooks/sentinel2.ipynb` for bulk runs. `README.md` is the
interface. This file is for work on the code.

## Layout

- `gum.c`: reads, crops, cloud share, references, plumes, workers, main.
- `nn.c`: the forward pass of `gum.bin`: packed GEMM over im2col,
  direct depthwise convolutions, then elementwise ops.
- `j2k.c`: burnoff's JPEG 2000 decoder (7f0deab). This copy adds 64x64
  code-blocks (10 m bands) and `red` (decode at resolution nl - red).
- `export.py`: model.pth to `gum.bin`, from the frozen TorchScript graph.
  Batch norm, then the activation, become the epilogue of the conv before
  them, in the same form as eager torch.
- `check.py`, `test.tsv`: `make check`.

## Rules

- Keep the notebook's maths. A change must pass `make check` with zero
  mismatches. Run `beetaloo/gum.sql` on the jump host after a change to
  reads: Shenandoah S1 on 2026-10-11 must give `max` 0.0976, the same as
  the torch run in `beetaloo/README.md`.
- Box: the 20 m pixel nearest to centre ± 2560 m. Bands are harmonised as
  `max(DN - 1000, 0)` from baseline 04.00 and scaled by `1 / 10000` in
  double, as the notebook does. Run mode needs N0400 or later, which has
  `MSK_CLASSI`.
- Markers: `peak_local_max` with distance 1 excludes the border and keeps
  every plateau pixel. The watershed result is binary, so the mask is the
  4-connected components over 0.1522 that hold a marker. Plumes are the
  8-connected regions of the mask, in raster order.
- Length: shapely 2.0 with GEOS 3.11 uses its own minimum-area rectangle.
  It goes over the GEOS hull edges (clockwise from the lowest, then
  leftmost, vertex) and takes the first minimum. `axis()` copies this.
- x86 flushes subnormals (MXCSR). Without this, SiLU tails slow the
  pointwise convs by up to 10×.

## Reads

A scene uses these rounds:

1. The first 64 KB of each band, and the whole mask.
2. Tile-parts. With a TLM, the positions are exact. Without a TLM, gum
   reads windows where the tile-part should be, from the nearest known
   neighbour plus layout lengths, and finds SOT markers (`scan()`). Each
   SOT must agree with the next SOT.
3. A 10 m band first reads two thirds of a tile-part. Then the PLT gives
   the bytes needed (`upto()`).

`lays` keeps the last file's layout for each MGRS tile and band, shared
by the workers. A second site on the same file reads exact ranges. On
eodata, the budget is 5000 requests a minute for each key, and other
services on the jump host share it.

## Measured

- Model on the jump host: 1.0 s for each target on one core. GEMM blocking
  of MR = 8 and NR = 8 is the fastest that GCC gives. A 6×16 AVX2
  intrinsics kernel was not faster.
- 180 rows (4 sites, Aug–Oct 2025, N0511) with 4 workers took 118 s while
  the host was shared with mars-s2l.

## Where it runs

The Data Desk runs gum as `fetch@gum` (`etl/fetch/gum` in
data-desk-eco/data-desk) on the jump host, from `/srv/eucalyptus`. That
checkout moves only when it is deployed: `git -C /srv/eucalyptus pull`.
The next run builds `gum` and `gum.bin` again.
