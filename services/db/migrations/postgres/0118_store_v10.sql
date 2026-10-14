-- 0118_store_v10: Krunker v10.0.2 prize wheels. The wheels and their item pools are compiled into
-- kn_brain (shared/include/store_spins.h: v10's gapi /data/spins); a row here replaces that set
-- without a release (wheel rotation: new pools, dates, prices). data = the /data/spins JSON
-- ({"season"?, "wheels": [...], "itemPools": {...}}), read by services/brain/store.c every
-- accounts.store_reload_ms; delete the row to go back to the built-in set. Additive: no existing
-- table or row changes (store indices 0-6447 keep their items; inventory rows are untouched).
-- Owner: svc-accounts.

CREATE TABLE store_spin_data (
    id         integer PRIMARY KEY CHECK (id = 1),
    data       text NOT NULL,
    updated_at timestamptz NOT NULL DEFAULT now()
);
