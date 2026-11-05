-- summary of gum's basin.jsonl: per pad, then each plume within 500 m of
-- the pad (the box centre in basin.tsv), by date
create macro hypot(a, b) as sqrt(a * a + b * b);
create table o as
  select *, strptime(regexp_extract(scene, '_(\d{8})T\d{6}_B12', 1),
                     '%Y%m%d')::date date
  from (from read_json('basin.jsonl', union_by_name = true)
        union all by name select null::varchar error where false);
create table c as select distinct column0 key, column1 cx, column2 cy
  from read_csv('basin.tsv', header = false, delim = '\t');
select split_part(key, '/', 1) pad, count(*) scenes, count(error) errors,
       count(refs) targets, count(*) filter (len(plumes) > 0) with_plumes,
       sum(len(plumes)) plumes, round(avg(reads)) reads,
       round(avg(mb), 1) mb, round(max(max), 3) max
from o group by all order by pad;
select split_part(key, '/', 1) pad, date, round(p.likelihood, 2) l,
       p.pixels, round(hypot(p.x - cx, p.y - cy)) dist_m,
       round(p.q / 1000, 2) "t/h per m/s"
from (select *, unnest(plumes) p from o) join c using (key)
where hypot(p.x - cx, p.y - cy) <= 500 order by pad, date;
