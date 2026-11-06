-- the targets for review.py, from gum's basin.jsonl and basin.tsv: plumes
-- within 500 m of a pad, the 8 strongest plumes, the persistent cluster
-- 1.6-2 km north of shenandoah s1, and the mars-s2l flag dates. writes
-- sel.json and crops.tsv (key, x, y, B12 of each target and reference)
create table o as
  select *, strptime(regexp_extract(scene, '_(\d{8})T\d{6}_B12', 1),
                     '%Y%m%d')::date date
  from read_json('basin.jsonl', union_by_name = true);
create table c as select distinct column0 key, column1 cx, column2 cy
  from read_csv('basin.tsv', header = false, delim = '\t');
create table p as select key, date, cx, cy, u.*
  from (select *, unnest(plumes) u from o) join c using (key);
create table s as
  select key, date, 'near pad' why from p
  where sqrt((x - cx) ^ 2 + (y - cy) ^ 2) <= 500
  union select key, date, 'top likelihood'
  from (from p order by likelihood desc limit 8)
  union select key, date, 'persistent n' from p
  where key like 'shenandoah-s1%' and round((x - cx) / 400) = 0
    and round((y - cy) / 400) in (4, 5)
  union select key, date, 'mars-s2l date' from o
  where refs is not null and date in ('2026-10-11', '2026-10-19', '2026-08-15');
copy (select key, string_agg(distinct why, '; ') why, cx, cy, any_value(o) r
      from s join o using (key, date) join c using (key)
      group by key, date, cx, cy order by key, date) to 'sel.json';
copy (select distinct key, cx, cy, unnest(list_prepend(r.scene, r.refs))
      from read_json('sel.json'))
  to 'crops.tsv' (delimiter '\t', header false);
