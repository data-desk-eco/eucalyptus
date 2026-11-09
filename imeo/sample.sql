-- 40 IMEO sentinel-2 plumes, 2023 to 2026: 8 in each of five rate bands,
-- one per 0.1 degree cell, drawn with a fixed seed. writes sample.csv
install httpfs; load httpfs;
copy (
with p as (
  from read_parquet('https://s3.WAW3-2.cloudferro.com/data-desk-archive/'
                    || 'imeo/detections/data.parquet')
  select id, date, lat, lon, rate_kg_h, wind_ms,
         case when rate_kg_h < 1500 then 1 when rate_kg_h < 3000 then 2
              when rate_kg_h < 6000 then 3 when rate_kg_h < 15000 then 4
              else 5 end band
  where satellite = 'S2' and kind = 'plume' and valid and year >= 2023
    and rate_kg_h is not null and wind_ms is not null
  qualify row_number() over (partition by round(lat, 1), round(lon, 1)
                             order by hash(id)) = 1)
select * exclude (r) from (
  select *, row_number() over (partition by band order by hash(id || 's'))
    r from p)
where r <= 8 order by band, date
) to 'sample.csv';
