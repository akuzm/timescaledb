-- This file and its contents are licensed under the Timescale License.
-- Please see the included NOTICE for copyright information and
-- LICENSE-TIMESCALE for a copy of the license.

\c :TEST_DBNAME :ROLE_SUPERUSER
-- helper function: float -> pseudorandom float [-0.5..0.5]
CREATE OR REPLACE FUNCTION mix(x anyelement) RETURNS float8 AS $$
    SELECT hashfloat8(x::float8) / pow(2, 32)
$$ LANGUAGE SQL;

\set CHUNKS 2::int
\set CHUNK_ROWS 100000::int
\set GROUPING_CARDINALITY 10::int

create table aggfns(t int, s int,
    cint2 int2, cint4 int4, cint8 int8,
    cfloat4 float4, cfloat8 float8,
    cts timestamp, ctstz timestamptz,
    cdate date);
select create_hypertable('aggfns', 's', chunk_time_interval => :GROUPING_CARDINALITY / :CHUNKS);

create view source as
select s * 10000::int + t as t,
    s,
    case when t % 1051 = 0 then null else (mix(s + t + 1) * 32767)::int2 end as cint2,
    (mix(s + t + 2) * 32767 * 65536)::int4 as cint4,
    (mix(s + t + 3) * 32767 * 65536)::int8 as cint8,
    case when s = 1 and t = 1061 then 'nan'::float4
        when s = 2 and t = 1061 then '+inf'::float4
        when s = 3 and t = 1061 then '-inf'::float4
        else (mix(s + t + 4) * 100)::float4 end as cfloat4,
    (mix(s + t + 5) * 100)::float8 as cfloat8,
    '2021-01-01 01:01:01'::timestamp + interval '1 second' * (s * 10000::int + t) as cts,
    '2021-01-01 01:01:01'::timestamptz + interval '1 second' * (s * 10000::int + t) as ctstz,
    '2021-01-01'::date + interval '1 day' * (s * 10000::int + t) as cdate
from
    generate_series(1::int, :CHUNK_ROWS * :CHUNKS / :GROUPING_CARDINALITY) t,
    generate_series(0::int, :GROUPING_CARDINALITY - 1::int) s(s)
;

insert into aggfns select * from source where s = 1;

alter table aggfns set (timescaledb.compress, timescaledb.compress_orderby = 't',
    timescaledb.compress_segmentby = 's');

select count(compress_chunk(x)) from show_chunks('aggfns') x;

alter table aggfns add column ss int default 11;
alter table aggfns add column x text default '11';

insert into aggfns
select *, ss::text as x from (
    select *,
        case
            -- null in entire batch
            when s = 2 then null
            -- null for some rows
            when s = 3 and t % 1053 = 0 then null
            -- for some rows same as default
            when s = 4 and t % 1057 = 0 then 11
            -- not null for entire batch
            else s
        end as ss
    from source where s != 1
) t
;
select count(compress_chunk(x)) from show_chunks('aggfns') x;
vacuum freeze analyze aggfns;


create table edges(t int, s int, ss int, f1 int);
select create_hypertable('edges', 't');
alter table edges set (timescaledb.compress, timescaledb.compress_segmentby='s');
insert into edges select
    s * 10000 + f1 as t,
    s,
    s,
    f1
from generate_series(0, 10) s,
    lateral generate_series(0, 60 + s + (s / 5::int) * 64) f1
;
select count(compress_chunk(x)) from show_chunks('edges') x;
vacuum freeze analyze edges;


set timescaledb.debug_require_vector_agg = 'require';
---- Uncomment to generate reference. Note that there are minor discrepancies
---- on float4 due to different numeric stability in our and PG implementations.
--set timescaledb.enable_vectorized_aggregation to off; set timescaledb.debug_require_vector_agg = 'allow';

select
    format('%sselect %s%s(%s)%s from aggfns%s%s order by 1;',
            explain,
            grouping || ', ',
            function, variable,
            ' filter (where ' || agg_filter || ')',
            ' where ' || condition,
            ' group by ' || grouping )
from
    unnest(array[
        'explain (costs off) ',
        null]) explain,
    unnest(array[
        't',
        's',
        'ss',
        'cint2',
        'cint4',
        'cint8',
        'cfloat4',
        'cfloat8',
        'cts',
        'ctstz',
        'cdate',
        '*']) variable,
    unnest(array[
        'min',
        'max',
        'sum',
        'avg',
        'stddev',
        'count']) function,
    unnest(array[
        null,
        'cfloat8 > 0',
        'cfloat8 <= 0',
        'cfloat8 < 1000' /* vectorized qual is true for all rows */,
        'cfloat8 > 1000' /* vectorized qual is false for all rows */,
        'cint2 is null']) with ordinality as condition(condition, n),
    unnest(array[
        null,
        's',
        'ss',
        'x']) with ordinality as grouping(grouping, n),
    unnest(array[
        null,
        'cint4 > 0']) with ordinality as agg_filter(agg_filter, n)
where
    true
    and (explain is null /* or condition is null and grouping = 's' */)
    and (variable != '*' or function = 'count')
    and (variable not in ('t', 'cts', 'ctstz', 'cdate') or function in ('min', 'max'))
    -- This is not vectorized yet
    and (variable != 'cint8' or function != 'stddev')
    and (function != 'count' or variable in ('cint2', 's', '*'))
    and (agg_filter is null or (function = 'count') or (function = 'sum' and variable in ('cint2', 'cint4')))
    and (condition is distinct from 'cint2 is null' or variable = 'cint2')
    -- No need to test the aggregate functions themselves again for string
    -- grouping.
    and (grouping is distinct from 'x' or (function = 'count' and variable in ('cint2', '*') and agg_filter is null))
order by explain, condition.n, variable, function, grouping.n, agg_filter.n
\gexec


-- Test multiple aggregate functions as well.
select count(*), count(cint2), min(cfloat4), cint2 from aggfns group by cint2
order by count(*) desc, cint2 limit 10
;

select s, count(*) from edges group by 1 order by 1;

select s, count(*), min(f1) from edges where f1 = 63 group by 1 order by 1;
select s, count(*), min(f1) from edges where f1 = 64 group by 1 order by 1;
select s, count(*), min(f1) from edges where f1 = 65 group by 1 order by 1;

select ss, count(*), min(f1) from edges where f1 = 63 group by 1 order by 1;
select ss, count(*), min(f1) from edges where f1 = 64 group by 1 order by 1;
select ss, count(*), min(f1) from edges where f1 = 65 group by 1 order by 1;