"""the object 1.69 km north of shenandoah s1 (gum's source pixel on 5
dates). for each clear scene of the pad (s1.tsv: gum jobs with bad <= 0.05)
prints the date and the object's contrast (3x3 mean / median of a 4-12 px
ring, -1) in B12/B11, B11, B12 and B04; then draws chips.png: true colour,
B12 B11 B8A and the B12/B11 change against REF, ±500 m, for DATES.
crops are gum's (gum MODEL B12 X Y) in s1/, named by md5(url)[:12].u16.
usage: uv run --with numpy --with matplotlib python north.py REF DATES..."""
import hashlib, sys, numpy as np, matplotlib
matplotlib.use('agg'); import matplotlib.pyplot as plt

r0, c0 = 43, 126   # (-30, 1690) m from the pad centre
y, x = np.ogrid[-r0:256 - r0, -c0:256 - c0]; d = np.hypot(y, x)
o, a = d <= 1.5, (d >= 4) & (d <= 12)
U = {l.split()[3].rsplit('_', 2)[1][:8]: l.split()[3] for l in open('s1.tsv')}
cr = lambda u: np.fromfile('s1/' + hashlib.md5(u.encode()).hexdigest()[:12]
                           + '.u16', '<u2').reshape(8, 256, 256) / 1e4
rat = lambda c: c[1] / np.maximum(c[0], 1e-4)
for k in sorted(U):
    c = cr(U[k]); f = lambda b: b[o].mean() / np.median(b[a]) - 1
    print(k, *(f'{f(b):.4f}' for b in (rat(c), c[0], c[1], c[5])), sep='\t')
R0, D = rat(cr(U[sys.argv[1]])), [k for k in sys.argv[2:] if k in U]
f, ax = plt.subplots(3, len(D), figsize=(2.2 * len(D), 7), squeeze=False)
w = np.s_[18:69, 101:152]
for i, k in enumerate(D):
    c = cr(U[k]); q = rat(c) / R0; q = q / np.median(q) - 1
    for p, im, kw in zip(ax[:, i], [
            np.clip(c[[5, 6, 7]].transpose(1, 2, 0)[w] / .25, 0, 1),
            np.clip(c[[1, 0, 2]].transpose(1, 2, 0)[w] / .5, 0, 1), q[w]],
            [{}, {}, dict(cmap='RdBu', vmin=-.08, vmax=.08)]):
        p.imshow(im, extent=(-530, 490, 1190, 2210), **kw)
        p.plot(-30, 1690, 'o', mfc='none', mec='lime', ms=10)
        p.set_xticks([]); p.set_yticks([])
    ax[0, i].set_title(k)
for p, t in zip(ax[:, 0], ['true colour', 'B12 B11 B8A',
                           f'B12/B11 vs {sys.argv[1]}']):
    p.set_ylabel(t)
f.tight_layout(); f.savefig('chips.png', dpi=70)
