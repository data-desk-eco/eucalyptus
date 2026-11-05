"""orbio's project eucalyptus sentinel-2 model on a crop.py stack, following
notebooks/sentinel2.ipynb: inference, watershed masks, major-axis flux with
the GEOS-FP wind. writes STACK.png (rgb, B12/B11 change against the nearer
reference, likelihood, conditional, marginal) and prints each plume.
usage: .venv/bin/python beetaloo/run.py STACK.npy [GEOS.nc4] (from the
repository; a GEOS-FP file in place of the download)."""
import sys, pathlib, numpy as np, pandas as pd, torch, matplotlib, xarray
from affine import Affine
from rasterio.crs import CRS
from skimage.measure import label
sys.path.insert(0, 'notebooks')
from utils import (calculate_major_axis_quantification,
                   download_wind_data_from_GEOS_FP, get_plume_source_location_v2,
                   get_s2_rgb_bands, get_wind_speed_for_plume,
                   mask_and_segment_plumes, predict)
import matplotlib.pyplot as plt

f = pathlib.Path(sys.argv[1]); d = np.load(f, allow_pickle=True).item()
main, (r1, r2) = d['main_image'], d['reference_images']
want = ['B11', 'B12', 'B8A', 'B07', 'B05', 'B04', 'B03', 'B02']
pick = lambda i: i['crop_arrays'][[i['bands'].index(b) for b in want]]
x = torch.from_numpy(np.concatenate([pick(r1), pick(main), pick(r2)]) / 1e4)
model = torch.load('notebooks/resources/s2/model.pth', weights_only=False,
                   map_location='cpu')
y = predict(model, x.float()[None])
p = y.sel(prediction='likelihood').values
cond = y.sel(prediction='conditional').values
marg = y.sel(prediction='marginal').values
mask, regions = mask_and_segment_plumes(
    p, marker_distance=1, marker_threshold=0.3044,
    watershed_floor_threshold=0.1522, closing_footprint_size=0)
lut = pd.read_csv('notebooks/resources/s2/radtran_lookup_table.csv')
ret = np.interp(-cond, -lut.frac, lut.ch4) * p  # marginal, mol/m2
crs, tr = CRS.from_string(d['crs']), Affine(*d['transform'][:6])
wind = (xarray.open_dataset(sys.argv[2]) if len(sys.argv) > 2
        else download_wind_data_from_GEOS_FP(main['date']))
lab = label(mask, connectivity=2)
print(main['id'], '| refs', r1['id'][:3], r1['date'][:10],
      r2['id'][:3], r2['date'][:10], '| max likelihood', f'{p.max():.3f}')
for g in regions:
    a, b, c, e = g.bbox; m = lab[a:c, b:e] == g.label
    pm = p[lab == g.label].max(); s = ret[a:c, b:e] / pm
    lat, lon = get_plume_source_location_v2(s * m, crs,
                                            tr * Affine.translation(b, a))
    ue, uh, ul, wd, u10 = get_wind_speed_for_plume(wind, lat, lon)
    L, ime, q = calculate_major_axis_quantification(m, s, 20.0, ue)
    print(f'plume {g.label}: source {lat:.5f} {lon:.5f}, {m.sum()} px, '
          f'likelihood {pm:.2f}, {q:.0f} kg/h, wind from {wd:.0f} deg')

ratio = lambda i: i['crop_arrays'][i['bands'].index('B12')] / \
    np.maximum(i['crop_arrays'][i['bands'].index('B11')], 1)
dr = ratio(main) / ratio(r1); dr = dr / np.nanmedian(dr) - 1
ext = (-2560, 2560, -2560, 2560)
rgb = get_s2_rgb_bands(main['crop_arrays'], main['bands'])
panels = [(rgb, {}, 'true colour'),
          (dr, dict(cmap='RdBu', vmin=-.08, vmax=.08),
           f"B12/B11 vs {r1['date'][:10]}, -1"),
          (p, dict(cmap='hot_r', vmin=0, vmax=1), 'likelihood'),
          (cond, dict(cmap='RdBu_r', vmin=-.08, vmax=.08), 'conditional (frac)'),
          (marg, dict(cmap='RdBu_r', vmin=-.08, vmax=.08), 'marginal (frac)')]
fig, ax = plt.subplots(1, 5, figsize=(22, 4.8), facecolor='black')
for g, (img, kw, t) in zip(ax, panels):
    g.imshow(img, extent=ext, **kw)
    g.contour(mask, [.5], colors='c', linewidths=.8, extent=(-2560, 2560,
                                                             2560, -2560))
    g.plot(0, 0, 'o', mfc='none', mec='lime', ms=10, mew=1.2)
    g.set_xticks([]); g.set_yticks([]); g.set_title(t, color='w', fontsize=10)
fig.suptitle(f"{main['id']} · project eucalyptus · 5.12 km box, pad ringed, "
             f"watershed mask cyan · max likelihood {p.max():.2f}", color='w')
fig.tight_layout()
fig.savefig(f.with_suffix('.png'), dpi=100, facecolor='black')
