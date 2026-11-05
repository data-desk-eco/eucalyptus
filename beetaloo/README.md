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
