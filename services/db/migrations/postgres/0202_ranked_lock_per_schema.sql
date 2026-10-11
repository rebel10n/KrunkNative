-- 0202: the ranked matcher's advisory lock key depends on the schema, so
-- deployments/tests that share one database through different schemas
-- (database.search_path) do not serialise each other's matchers.
-- Owner: svc-matchmaker.
CREATE OR REPLACE FUNCTION kn_rq_lock() RETURNS bigint LANGUAGE sql STABLE AS $$
    SELECT (74610201::bigint << 32) # (hashtext(current_schema())::bigint & 4294967295)
$$;
