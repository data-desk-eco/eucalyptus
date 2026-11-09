# gum against IMEO's Sentinel-2 plumes

A test of gum's recall on plumes another Sentinel-2 method found. The
sample is 40 IMEO plumes from 2023 to 2026, 8 in each of five rate bands,
one for each 0.1° cell (`sample.sql`, which writes `sample.csv`). For
each plume, `jobs.sql` makes jobs on the jump host: every scene of one
tile in the 60 days to the plume's date under 50% cloud, and the date
itself.

```bash
for id in ...; do duckdb ... -f jobs.sql | cat >> jobs.tsv; done
GUM_THREADS=2 gum/gum gum/gum.bin < jobs.tsv > out.jsonl  # with eodata
duckdb < eval.sql   # writes eval.csv
```

A plume is found if a gum plume's source is within 1 km of IMEO's
source on the same date. gum's rate is `q` times IMEO's wind speed.

| Rate band, kg/h | Plumes | Clear | Found | Median max | gum / IMEO rate |
| --- | --- | --- | --- | --- | --- |
| under 1,500 | 8 | 7 | 4 | 0.56 | 0.57 |
| 1,500–3,000 | 8 | 8 | 8 | 1.00 | 0.59 |
| 3,000–6,000 | 8 | 8 | 7 | 0.99 | 0.65 |
| 6,000–15,000 | 8 | 7 | 7 | 0.88 | 0.48 |
| 15,000 or more | 8 | 8 | 8 | 1.00 | 0.55 |
| All | 40 | 38 | 34 | 0.99 | 0.61 |

- 28 of the 40 sources are within 500 m, and 31 of the 34 matches have a
  likelihood of 0.5 or more.
- By chance: over the Silesian coal-mine run (2,808 clear targets), a gum
  plume falls within 1 km of the box centre in 0.4% of targets.
- gum's rate is about 0.6 times IMEO's, with the same wind.
- The sample has a bias. IMEO found these plumes in Sentinel-2, and most
  are on dry, bare ground (Turkmenistan, Algeria, Libya, the Permian).

## Silesia

Over the Silesian coal mines (`~/Research/coal-mine-drainage`), other
satellites saw 28 plumes of 2 t/h or more on clear gum dates (within one
day). gum's largest likelihood in the box was 0.30 or less on each. In
the IMEO sample, gum found 15 of 16 plumes of 1.5–6 t/h. Thus, the limit
at Silesia is the surface (fields, forest and towns that change between
scenes), not the rate.
