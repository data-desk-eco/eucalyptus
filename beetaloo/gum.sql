-- gum jobs (gum/README.md) for the scenes mars-s2l flagged: each pad's scenes on the
-- flagged tile over the 90 days to the flagged date, under 50% cloud (and
-- the flagged one), oldest first
install spatial; load spatial;
copy (
with f(pad, id, lat, lon) as (values
  ('shenandoah-s1', 'S2A_MSIL1C_20261011T011741_N0513_R088_T53KLB_20261011T030204', -16.841469, 133.651876),
  ('carpentaria-4', 'S2C_MSIL1C_20261019T011721_N0513_R088_T53KNB_20261019T024843', -16.858125, 135.171867),
  ('shenandoah-s2', 'S2B_MSIL1C_20260815T011719_N0512_R088_T53KLB_20260815T024518', -16.804125, 133.642389)),
m as (from read_json('/data/s2e/beetaloo/*.scenes.gz', filename = true,
        format = 'newline_delimited', union_by_name = true) where id is not null),
s as (select f.pad, f.lat, f.lon, m.*, g.datetime gt from f
      join m g on g.id = f.id and g.filename like '%/' || f.pad || '.scenes.gz'
      join m on m.filename = g.filename and m.mgrs = g.mgrs
       and m.datetime <= g.datetime
       and m.datetime::timestamp > g.datetime::timestamp - interval 90 day
       and (m.cloud_cover < 50 or m.id = g.id)
      qualify row_number() over (partition by f.pad, m.datetime
                                 order by m.id desc) = 1),
p as (select *, ST_Transform(ST_Point(lon, lat), 'EPSG:4326',
                             'EPSG:' || epsg, always_xy := true) g from s)
select pad, round(ST_X(g) / 20) * 20, round(ST_Y(g) / 20) * 20, bands.b12
from p order by pad, datetime
) to '/dev/stdout' (delimiter '\t', header false);
