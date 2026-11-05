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
