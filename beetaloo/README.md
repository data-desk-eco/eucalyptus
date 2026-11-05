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
