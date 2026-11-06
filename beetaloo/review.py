"""panels for gum targets: true colour, B12/B11 change against each
reference, and the model likelihood, with gum's plume sources (circles) and
the pad (cross); the second row is ±600 m around the strongest plume, with
the change over 5x5 pixels at ±2%. crops are gum's (gum MODEL B12 X Y),
named by md5(key + url)[:12].u16. run from the repository:
usage: .venv/bin/python beetaloo/review.py SEL.json CROPS OUT"""
import hashlib, json, sys, pathlib, numpy as np, torch, matplotlib
sys.path.insert(0, 'notebooks')
from utils import predict
from scipy.ndimage import uniform_filter
matplotlib.use('agg'); import matplotlib.pyplot as plt

sel, crops, out = sys.argv[1], *map(pathlib.Path, sys.argv[2:4])
out.mkdir(exist_ok=True)
m = torch.load('notebooks/resources/s2/model.pth', weights_only=False,
               map_location='cpu')
crop = lambda k, u: np.fromfile(crops / (hashlib.md5((k + u).encode())
    .hexdigest()[:12] + '.u16'), '<u2').reshape(8, 256, 256) / 1e4
day = lambda u: u.rsplit('_', 2)[1][:8]
rat = lambda c: c[1] / np.maximum(c[0], 1e-4)
ext = (-2560, 2560, -2560, 2560)
for s in map(json.loads, open(sel)):
    k, r = s['key'], s['r']; t, a, b = (crop(k, u) for u in
                                        [r['scene'], *r['refs']])
    p = predict(m, torch.from_numpy(np.concatenate([a, t, b])).float()[None]
                ).sel(prediction='likelihood').values
    dr = [rat(t) / rat(c) for c in (a, b)]
    panels = [(np.clip(t[[5, 6, 7]].transpose(1, 2, 0) / .25, 0, 1), {},
               'true colour')] + [
        (d / np.nanmedian(d) - 1, dict(cmap='RdBu', vmin=-.1, vmax=.1),
         f'B12/B11 vs {day(u)}') for d, u in zip(dr, r['refs'])] + [
        (p, dict(cmap='hot_r', vmin=0, vmax=1), 'likelihood')]
    f, ax = plt.subplots(2, 4, figsize=(20, 10.4))
    z = max(r['plumes'], key=lambda q: q['likelihood'], default=None)
    for i, x, (im, kw, ti) in zip(range(8), ax.flat, panels * 2):
        if i > 3 and 'B12' in ti:
            im, ti = uniform_filter(im, 5), ti + ', 5x5'
            kw = kw | dict(vmin=-.02, vmax=.02)
        x.imshow(im, extent=ext, **kw); x.set_title(ti); x.plot(0, 0, 'g+',
                                                              ms=14, mew=2)
        if i > 3 and z: x.set(xlim=(z['x'] - s['cx'] - 600,
                                    z['x'] - s['cx'] + 600),
                              ylim=(z['y'] - s['cy'] - 600,
                                    z['y'] - s['cy'] + 600))
        for q in r['plumes']:
            x.add_patch(plt.Circle((q['x'] - s['cx'], q['y'] - s['cy']), 150,
                                   fill=False, ec='lime', lw=1.5))
            x.text(q['x'] - s['cx'] + 180, q['y'] - s['cy'],
                   f"{q['likelihood']:.2f}", color='lime', fontsize=9,
                   clip_on=True)
    pad = k.split('/')[0]
    f.suptitle(f"{pad} {r['date']} ({s['why']}), max {r['max']:.2f}, "
               f"bad {r['bad']:.2f}; m from pad")
    f.tight_layout(); f.savefig(out / f"{pad}_{r['date']}.png", dpi=70)
    plt.close(f); print(pad, r['date'], f'{p.max():.3f}', f"{r['max']:.3f}")
