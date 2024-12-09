-- This file and its contents are licensed under the Timescale License.
-- Please see the included NOTICE for copyright information and
-- LICENSE-TIMESCALE for a copy of the license.

CREATE TABLE IF NOT EXISTS timdat (
  dinr                  integer NOT NULL DEFAULT 0,
  lormnr                character(32) NOT NULL DEFAULT ' '::character(1),
  cibin                 character(16) NOT NULL DEFAULT ' '::character(1),
  tloan                 character(12) NOT NULL DEFAULT ' '::character(1),
  dust_timdat           character(48) NOT NULL DEFAULT ' '::character(1),
  PRIMARY KEY (dinr, lormnr, cibin)
);

INSERT INTO timdat (dinr,lormnr,cibin,tloan,dust_timdat)
SELECT
  floor(random() * 255 + 1)::int,
  upper(substr(md5(random()::text), 1, 32)),
  upper(substr(md5(random()::text), 1, 10)),
  upper(substr(md5(random()::text), 1, 10)),
  upper(substr(md5(random()::text), 1, 48))
FROM generate_series(1,3000) n
;

CREATE TABLE IF NOT EXISTS kendat (
  dinr                  integer NOT NULL DEFAULT 0,
  kind                  integer NOT NULL DEFAULT 0,
  dal                   character(15) NOT NULL DEFAULT ' '::character(1),
  cibin                 character(10) NOT NULL DEFAULT ' '::character(1),
  cutdat                character(10) NOT NULL DEFAULT ' '::character(1),
  PRIMARY KEY (dinr, cibin)
);
CREATE INDEX kendat_dinr_cutdat_idx ON kendat (dinr, cutdat);


INSERT INTO kendat (dinr,kind,dal,cibin,cutdat)
SELECT
  floor(random() * 255 + 1)::int,
  floor(random() * 255 + 1)::int,
  upper(substr(md5(random()::text), 1, 15)),
  upper(substr(md5(random()::text), 1, 10)),
  upper(substr(md5(random()::text), 1, 10))
FROM generate_series(1,3000) n
;


SELECT kendat.cibin, kendat.dal, kendat.cutdat
    FROM kendat
   WHERE kendat.dinr = 57
   AND kendat.kind = 179
     AND kendat.cibin NOT IN (SELECT DISTINCT timdat.cibin
    FROM timdat
   WHERE timdat.dinr =  57
   AND kendat.kind = 179
     AND timdat.lormnr = 'E1D299B260FB1C1A2A0196A6AADC039B');
