-- gum jobs for one sample.csv plume (variable id): every scene of its
-- tile in the 60 days to the plume's date under 50% cloud, and the date
-- itself, newest processing at N0400 or later, oldest first. one tile per
-- plume: of those with a scene on the date, the one with most scenes.
-- run on the jump host with burnoff's s2_scenes over eodata
copy (
with p as (from 'sample.csv' where id = getvariable('id')),
c as (select p.id pid, p.date, p.lat, p.lon, s.*,
             string_split(s.id, '_')[6] mgrs
      from p, s2_scenes(ST_Buffer(ST_Point(p.lon, p.lat), 1e-3),
                        p.date - 60, t1 := p.date, source := 'eodata') s
      where string_split(s.id, '_')[4] >= 'N0400'
        and (s.cloud < 50 or s.datetime::date = p.date)
      qualify row_number() over (partition by mgrs, datetime::date
                                 order by s.id desc) = 1),
t as (select mgrs from c group by mgrs
      having bool_or(datetime::date = date) order by count(*) desc, mgrs
      limit 1),
g as (select *, ST_Transform(ST_Point(lon, lat), 'EPSG:4326', crs,
                             always_xy := true) pt
      from c semi join t using (mgrs))
select pid || '/' || mgrs, round(ST_X(pt) / 20) * 20,
       round(ST_Y(pt) / 20) * 20, b12
from g order by datetime
) to '/dev/stdout' (delimiter '\t', header false);
