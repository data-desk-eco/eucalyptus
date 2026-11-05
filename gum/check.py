"""gum against the notebook: each target in gum's output read again as
gum's crops (gum MODEL B12 X Y), then notebooks/sentinel2.ipynb's steps
in torch, numpy and skimage, field by field. run from the repository:
usage: check.py GUM MODEL JOBS.tsv OUT.jsonl"""
import json, subprocess, sys, numpy as np, pandas as pd, torch
from affine import Affine
from rasterio.transform import AffineTransformer
from skimage.measure import label
sys.path.insert(0, 'notebooks')
from utils import (calculate_major_axis_quantification,
                   mask_and_segment_plumes, predict)

gum, model, jobs, out = sys.argv[1:5]
at = {l.split('\t')[3].strip(): tuple(map(float, l.split('\t')[1:3]))
      for l in open(jobs)}
m = torch.load('notebooks/resources/s2/model.pth', weights_only=False,
               map_location='cpu')
lut = pd.read_csv('notebooks/resources/s2/radtran_lookup_table.csv')
crop = lambda u: np.frombuffer(subprocess.run(
    [gum, model, u, *map(str, at[u])], check=True, capture_output=True
).stdout, '<u2').reshape(8, 256, 256)
bad = 0
for r in map(json.loads, open(out)):
    if 'plumes' not in r: continue
    x = np.concatenate([crop(r['refs'][0]), crop(r['scene']),
                        crop(r['refs'][1])]) * (1 / 10000)
    y = predict(m, torch.from_numpy(x).float()[None])
    p = y.sel(prediction='likelihood').values
    cond = y.sel(prediction='conditional').values
    mask, regions = mask_and_segment_plumes(p, 1, 0.3044, 0.1522, 0)
    ret = np.interp(-cond, -lut.frac, lut.ch4) * p
    lab = label(mask, connectivity=2)
    cx, cy = at[r['scene']]
    # the box's corner, as gum puts it (tile corners are on the 20 m grid)
    tr = Affine(20, 0, 20 * np.floor((cx - 2560) / 20 + .5), 0, -20,
                -20 * np.floor((-cy - 2560) / 20 + .5))
    want = []
    for g in regions:
        a, b, c, e = g.bbox
        pm = np.max(p[lab == g.label])
        s, pl = ret[a:c, b:e] / pm, lab[a:c, b:e] == g.label
        i = np.unravel_index((s * pl).argmax(), s.shape)
        sx, sy = AffineTransformer(tr * Affine.translation(b, a)).xy(*i)
        L, ime, q = calculate_major_axis_quantification(pl, s, 20.0, 1)
        want.append(dict(x=sx, y=sy, pixels=int(pl.sum()), likelihood=pm,
                         ime=ime, length=L, q=q))
    print(r['scene'].rsplit('/', 1)[-1], f"max {p.max():.6f}", 'torch',
          len(want), 'plumes, gum', len(r['plumes']))
    if len(want) != len(r['plumes']): bad += 1
    for w, g in zip(want, r['plumes']):
        d = {k: g[k] - w[k] for k in w}
        print('  ', {k: f'{w[k]:.6g}' for k in w}, '\n   diff',
              {k: f'{v:.2g}' for k, v in d.items()})
        bad += any(abs(v) > 1e-4 * max(1, abs(w[k])) for k, v in d.items())
print('mismatches', bad)
sys.exit(bad > 0)
