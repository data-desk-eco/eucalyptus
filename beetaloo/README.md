# Beetaloo test

A test of Orbio's Sentinel-2 model on the Shenandoah S1 pad, 11 October
2026. MARS-S2L (s2e) flagged a plume there at 2.9 t/h.

1. `crop.py` runs on the jump host, where the eodata key is. It reads the
   s2e scene manifest and writes the target and its two nearest earlier
   scenes under 5% cloud: 256 x 256 crops at 20 m of all L1C bands, with the
   1000 DN offset removed.
2. `run.py` follows `notebooks/sentinel2.ipynb`: inference, watershed masks
   and major-axis flux. A local GEOS-FP file can replace the download.

```bash
uv venv --python 3.10 .venv && uv pip install -p .venv -r requirements.txt
.venv/bin/python beetaloo/run.py beetaloo/s1_20261011.npy \
  beetaloo/geos_20261011_0130.nc4
```

Result: references 4 Oct and 29 Sep 2026. The maximum likelihood in the
5.12 km box is 0.098, under the 0.30 marker threshold, so there is no plume.
The same code on the notebook's Hassi Messaoud stack gives a maximum
likelihood of 1.0 and 3,319 pixels over 0.30.

## gum

`gum.sql` makes `gum` jobs for the three scenes where mars-s2l flagged
plumes, run on the jump host from the s2e scene manifests
(`/data/s2e/beetaloo`). Each job covers the 90 days to the flagged date.

```bash
/opt/burnoff/duckdb < beetaloo/gum.sql > jobs.tsv
. /etc/profile.d/eodata.sh && gum/gum gum/gum.bin < jobs.tsv > out.jsonl
```

| Pad, date | mars-s2l | gum (Orbio) |
| --- | --- | --- |
| Shenandoah S1, 2026-10-11 | 2 plumes, 2.9 t/h, p 0.79 | max likelihood 0.098, no plume |
| Carpentaria 4, 2026-10-19 | 1 plume, 17.9 t/h, p 0.98 | max likelihood 0.12, no plume |
| Shenandoah S2, 2026-08-15 | 1 plume, 110 t/h, p 0.99 | 2 weak plumes (likelihood 0.32 and 0.39), 2.1–2.3 km from the pad |

The Shenandoah S1 likelihood is the same as the torch run above. The run
of 67 scenes took 43 s with 3 workers.

## gum over the basin

`basin.sql` makes `gum` jobs for every pad in `queue.csv`: all scenes of
each pad's manifest under 50% cloud. On 5 November 2026 this run replaced
the mars-s2l service (`s2e-beetaloo`, now stopped) on the jump host. The
output is `~/gum/basin.jsonl` on the jump host. `basin_summary.sql` makes
the tables below.

```bash
/opt/burnoff/duckdb < beetaloo/basin.sql > basin.tsv
. /etc/profile.d/eodata.sh && GUM_THREADS=3 gum/gum gum/gum.bin \
  < basin.tsv > basin.jsonl
duckdb < beetaloo/basin_summary.sql
```

The 1,910 scenes took 18 minutes with 3 workers (41 CPU minutes). One
scene failed: `tile-part not found` on S2B 2017-12-30 (53KNB, N0500).

| Pad | Scenes | Targets | With plumes | Plumes | Max likelihood |
| --- | --- | --- | --- | --- | --- |
| carpentaria-1 | 387 | 380 | 122 | 173 | 0.89 |
| carpentaria-2 | 534 | 525 | 142 | 174 | 0.91 |
| carpentaria-4 | 269 | 262 | 86 | 134 | 0.98 |
| shenandoah-s1 | 529 | 520 | 155 | 240 | 0.97 |
| shenandoah-s2 | 191 | 186 | 64 | 96 | 0.87 |

Plumes with a source within 500 m of the pad (`q` in t/h per m/s of wind):

| Pad | Date | Likelihood | Pixels | Distance, m | q |
| --- | --- | --- | --- | --- | --- |
| carpentaria-1 | 2020-06-27 | 0.48 | 137 | 216 | 0.47 |
| carpentaria-1 | 2023-10-30 | 0.43 | 185 | 455 | 0.46 |
| carpentaria-1 | 2025-07-08 | 0.36 | 156 | 326 | 0.47 |
| carpentaria-2 | 2024-08-20 | 0.42 | 101 | 355 | 0.28 |
| carpentaria-4 | 2025-10-24 | 0.32 | 126 | 416 | 0.62 |
| carpentaria-4 | 2026-05-17 | 0.34 | 186 | 297 | 0.74 |
| shenandoah-s1 | 2026-04-22 | 0.32 | 54 | 374 | 1.18 |
| shenandoah-s2 | 2023-11-04 | 0.32 | 59 | 91 | 0.26 |
| shenandoah-s2 | 2026-01-02 | 0.37 | 167 | 425 | 0.70 |

Shenandoah S1 on 2026-10-11 gives `max` 0.0976 again. All plumes near a
pad are weak (likelihood under 0.5). The 197 targets with a likelihood
of 0.5 or more have their plumes more than 500 m from the pad.

### Review

`review.sql` picks 42 targets from the basin run: the 9 plumes within
500 m of a pad, the 8 strongest plumes, the cluster 1.6–2 km north of
Shenandoah S1, and every pad on the mars-s2l dates. `crops.tsv` lists the
target and reference crops, which `gum gum.bin B12 X Y` makes on the jump
host. `review.py` then runs the model in torch and draws true colour, the
B12/B11 change against each reference and the likelihood. A second row
shows ±600 m around the strongest plume, with the change averaged over
5 × 5 pixels. In RdBu, red is a B12/B11 decrease, which is the methane
direction. Torch gives the same `max` as gum on all 42 targets.

```bash
duckdb < beetaloo/review.sql   # in the directory of basin.jsonl, basin.tsv
.venv/bin/python beetaloo/review.py sel.json crops png
```

Findings:

- No pad shows more plumes than the rest of the box. Sources within
  500 m of a pad occur at 11.5 per km², against 31–42 per km² at
  0.5–2.5 km.
- gum screens the references for cloud, but not the target. Targets with
  more than 5% bad pixels are 246 of 1,873, with 145 plumes. Clear targets
  still have plumes in 30% of scenes (485 of 1,627).
- The strongest plumes in clear scenes (Carpentaria 4 2022-07-27,
  Shenandoah S1 2022-04-18 and 2017-11-10) have a B12/B11 decrease of 1–2%
  against both references. Each decrease is a compact patch of 100–200 m.
  None has a downwind shape, and none starts at a pad or other structure.
- The most persistent cluster, 1.7 km north of Shenandoah S1 (13 dates in
  two 400 m cells), sits on a small bright object east of the road. On
  2020-01-29 (likelihood 0.92) the decrease is a 100 m patch on that
  object. This is more likely a surface or registration effect than a
  source, but an operator map could show a structure there.
- The near-pad plume at Shenandoah S2 on 2023-11-04 is before the queue
  start for that pad (2024-01-01). The near-pad plume at Carpentaria 1 on
  2020-06-27 is in one reference only and is as strong as its
  surroundings.
- mars-s2l dates: at Shenandoah S1 (2026-10-11) and Shenandoah S2
  (2026-08-15), the change maps show only the pad structures. At
  Carpentaria 4 (2026-10-19), a sharp 900 m band of B12/B11 decrease
  goes NNW from the pad. The model gives it 0.12. The band is also there
  on 10-24 and 11-03 against 10-04, but not on 10-24 against 10-19. Thus
  it is a surface change that started between 4 and 19 October (possibly a
  burn), not a plume.
- On Carpentaria 4 2022-07-27, the pad has a strong B12/B11 increase
  against both references, which agrees with flare heat. The model does
  not flag it.

The bulk run screens fast, but its plumes need review. With these pads, a
likelihood over 0.9 in a clear scene did not give a plume shape.
