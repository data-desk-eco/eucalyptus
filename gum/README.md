# gum

A C version of the Sentinel-2 detector in `notebooks/sentinel2.ipynb`, for
bulk runs. gum reads the parts of the L1C JPEG 2000 files that a 5.12 km
box needs, with HTTP range reads from CloudFerro eodata, Google's mirror or
local files. It runs Orbio's model, then applies the notebook's watershed
masks and major-axis quantification. gum has no Python, torch or GDAL
dependency at run time. It needs only libcurl.

## Build

```sh
make          # gum, and gum.bin (the model, exported from model.pth by uv)
make check    # the notebook's three scenes, against the notebook's own code
```

On Ubuntu, install `libcurl4-openssl-dev`. To export `gum.bin`, `uv` must
be installed. You can copy `gum.bin` to a host that does not have `uv`.

## Run

```sh
. /etc/profile.d/eodata.sh   # AWS_ACCESS_KEY_ID, AWS_SECRET_ACCESS_KEY, AWS_S3_ENDPOINT
./gum gum.bin < jobs.tsv > out.jsonl
```

Each line of `jobs.tsv` is `key`, `x`, `y` and `b12`, separated by tabs:

- `x` and `y` give the box centre in metres in the scene's UTM zone. Give
  values on the 20 m grid. Other values move to the nearest 20 m pixel.
- `b12` is the URL of the scene's B12 JP2: `s3://eodata/...`, `https://` or
  `file://`. gum finds the other bands and `MSK_CLASSI_B00.jp2` from the
  SAFE path.
- The lines of one key make one site's time series on one MGRS tile,
  oldest first. Each scene is a target. Its references are the two latest
  earlier scenes of the key that have at most 5% bad pixels in the box. A
  bad pixel is opaque cloud or cirrus in the L1C mask, or B12 no data. This
  copies the selection rule in `docs/Data.md`, but uses the L1C mask in
  place of OmniCloudMask.

gum writes one JSON line for each job: `bad` (the share of bad pixels in
the box), `reads` and `mb` (the requests and bytes it used), and an `error`
if the job failed. A target with references also has `refs`, `max` (the
largest likelihood in the box) and `plumes`. Each plume has these fields:

| Field | Meaning |
| --- | --- |
| `x`, `y` | Source pixel centre, UTM metres |
| `pixels` | Mask pixels (8-connected region) |
| `likelihood` | Largest likelihood in the plume |
| `ime` | Integrated mass enhancement, mol |
| `length` | Major axis, m |
| `q` | Emission rate per m/s of wind, kg/h |

To get the notebook's emission rate, multiply `q` by the GEOS-FP 10 m wind
speed at the source (`hypot(U10M, V10M)`). Use 0.5 and 1.5 times that
speed for the low and high bounds. gum leaves out the wind.

`gum gum.bin B12 X Y > crop.u16` writes one scene's model input:
8 × 256 × 256 little-endian uint16 values in the order B11, B12, B8A, B07,
B05, B04, B03, B02.

Use `GUM_THREADS` to set the number of workers. The default is one for
each core. Each worker runs one key at a time and reads the next scene
while the model runs.

## Exactness

- Crops: on the notebook's 2019 scene (baseline 02.08), all 8 bands are
  equal to the notebook's stack, value for value. The 10 m bands use the
  JP2's first reduced resolution, as GDAL reads them at 20 m.
- Model: on the notebook's stack, the largest likelihood difference from
  torch is 1e-5. The conditional difference is 1e-7. The same 3,306
  pixels are over 0.3044.
- Plumes: on 9 targets at Hassi Messaoud (October–November 2019,
  baseline 05.00), `check.py` found the same 25 plumes as the notebook's
  code. Pixels and sources are equal. Lengths agree to 1e-6 m and IME to
  1e-6 relative.

## Speed

These times are for a 256 × 256 box.

- Model: 0.33 s on one Apple M2 core. 1.0 s on one core of the jump host
  (Broadwell, 4 vCPUs). Torch takes 0.44 s on four M2 cores.
- Reads from eodata, measured on the jump host:
  - Baselines 05.12 and later have a TLM. A scene takes 17–31 requests
    and 5–14 MB.
  - Older baselines have no TLM. gum searches for tile-parts near where
    the last date of the tile had them. A scene then takes about 33
    requests and 42 MB.
