ALTER TABLE _timescaledb_internal.bgw_job_stat_history
    ALTER COLUMN succeeded DROP NOT NULL,
    ALTER COLUMN succeeded DROP DEFAULT;

CREATE OR REPLACE FUNCTION _timescaledb_functions.ts_bloom1_matches(bytea, anyelement)
RETURNS bool
AS '@MODULE_PATHNAME@', 'ts_update_placeholder'
LANGUAGE C IMMUTABLE STRICT;

