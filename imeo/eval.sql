-- gum against the IMEO sample: on each plume's date, the target's bad
-- share and max likelihood, and the gum plume whose source is nearest the
-- IMEO source (found if within 1 km). gum's rate uses IMEO's wind
create table o as
  select split_part(key, '/', 1) id, *,
         strptime(regexp_extract(scene, '_(\d{8})T\d{6}_B12', 1),
                  '%Y%m%d')::date d
  from (from read_json('out.jsonl', union_by_name = true)
        union all by name select null::varchar error where false);
create table c as select distinct split_part(column0, '/', 1) id,
  column1 cx, column2 cy from read_csv('jobs.tsv', header = false,
                                       delim = '\t');
create table r as
select s.band, s.id, s.date, s.lat, s.lon, s.rate_kg_h, s.wind_ms,
       o.error, o.bad, o.refs is not null has_refs, o.max,
       len(o.plumes) n, p.m, p.likelihood, p.pixels, p.q * s.wind_ms gum_kg_h
from 'sample.csv' s join c using (id)
left join o on o.id = s.id and o.d = s.date
left join lateral (
  select sqrt((u.x - cx) ^ 2 + (u.y - cy) ^ 2) m, u.*
  from (select unnest(o.plumes) u) order by m limit 1) p on true;
copy r to 'eval.csv';
select band, count(*) plumes, count(*) filter (has_refs) targets,
       count(*) filter (has_refs and bad <= 0.05) clear,
       count(*) filter (m <= 1000) found,
       count(*) filter (m <= 1000 and bad <= 0.05) found_clear,
       round(median(max), 2) med_max,
       round(median(gum_kg_h / rate_kg_h) filter (m <= 1000), 2) rate_ratio
from r group by rollup (band) order by band;
from r select band, date, round(lat, 3) lat, round(lon, 3) lon,
  rate_kg_h, round(bad, 2) bad, round(max, 2) max, round(m) m,
  round(likelihood, 2) l, pixels, round(gum_kg_h) gum_kg_h, error
order by band, date;
