-- gum jobs (gum/README.md) for every pad in queue.csv: all scenes of each
-- pad's manifest under 50% cloud, one key per pad and MGRS tile, oldest first
install spatial; load spatial;
copy (
with f as (from read_csv('/data/s2e/beetaloo/queue.csv', header = false,
             names = ['pad', 'lat', 'lon', 't0'], all_varchar = true,
             null_padding = true) where t0 is not null),
m as (from read_json('/data/s2e/beetaloo/*.scenes.gz', filename = true,
        format = 'newline_delimited', union_by_name = true)
      where id is not null and cloud_cover < 50),
s as (select f.pad, f.lat::double lat, f.lon::double lon, m.* from f
      join m on m.filename like '%/' || f.pad || '.scenes.gz'
      qualify row_number() over (partition by f.pad, m.mgrs, m.datetime
                                 order by m.id desc) = 1),
p as (select *, ST_Transform(ST_Point(lon, lat), 'EPSG:4326',
                             'EPSG:' || epsg, always_xy := true) g from s)
select pad || '/' || mgrs, round(ST_X(g) / 20) * 20, round(ST_Y(g) / 20) * 20,
       bands.b12
from p order by pad, mgrs, datetime
) to '/dev/stdout' (delimiter '\t', header false);
