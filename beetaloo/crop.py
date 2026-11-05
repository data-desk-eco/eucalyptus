"""a project eucalyptus input stack from a s2e scene manifest: the target
scene and the two nearest earlier scenes under CLOUD % (default 5), each a
256 x 256 crop at 20 m of every L1C band on LAT LON, harmonised (DN - 1000
from baseline 04.00), as notebooks/resources/s2/model_input_stack.npy.
usage: crop.py MANIFEST SCENE_ID LAT LON OUT [CLOUD] (run with eodata)."""
import gzip, json, sys, numpy as np, rasterio
from rasterio.windows import from_bounds
from rasterio.warp import transform

man, sid, lat, lon, out = sys.argv[1:6]
cloud = float(sys.argv[6]) if len(sys.argv) > 6 else 5
items = [json.loads(l) for l in list(gzip.open(man, 'rt'))[1:]]
s = next(i for i in items if i['id'] == sid)
refs = sorted((i for i in items if i['mgrs'] == s['mgrs']
               and i['date'] < s['date'] and (i['cloud_cover'] or 0) <= cloud),
              key=lambda i: i['date'], reverse=True)[:2]
(x,), (y,) = transform('EPSG:4326', f"EPSG:{s['epsg']}",
                       [float(lon)], [float(lat)])
x, y = round(x / 20) * 20, round(y / 20) * 20  # on the 20 m grid
box = (x - 2560, y - 2560, x + 2560, y + 2560)
bands = ['B01', 'B02', 'B03', 'B04', 'B05', 'B06', 'B07', 'B08', 'B09',
         'B11', 'B12', 'B8A']

def stack(i):
    off = 1000 if int(i['id'].split('_')[3][1:]) >= 400 else 0
    a = []
    for b in bands:
        href = '/vsis3/' + i['bands'][b.lower()].removeprefix('s3://')
        with rasterio.open(href) as r:
            v = r.read(1, window=from_bounds(*box, r.transform),
                       out_shape=(256, 256), boundless=True,
                       resampling=rasterio.enums.Resampling.average)
        a.append(np.clip(v.astype('i4') - off, 0, None).astype('u2'))
    return {'crop_arrays': np.stack(a), 'bands': bands, 'date': i['datetime'],
            'id': i['id']}

np.save(out, {'main_image': stack(s), 'reference_images': list(map(stack, refs)),
              'crs': f"EPSG:{s['epsg']}", 'bounds': box,
              'transform': tuple(rasterio.transform.from_bounds(*box, 256, 256))},
        allow_pickle=True)
print(s['id'], '| refs', [r['id'] for r in refs])
