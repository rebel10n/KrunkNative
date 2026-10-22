-- 0352_skin_copies (PostgreSQL): owned copies that carry a paint (v10.0.3's owned entries: id = the copy's id,
-- m = {paint, paintShader}; doc/v10/paint-copy.md). inventory.count still counts every owned unit, painted or
-- not, so ownership checks are unchanged; a row here is one painted unit of (account, skin). kn_add_skin refuses
-- to take away a painted unit (only the unpainted ones are sold, listed, traded or crafted). The copy id starts
-- at 1000000: a stack's sid on the market rows is its store index (below 1000000), so the two never meet.
-- Additive, plus kn_add_skin (0103) with that guard.
CREATE TABLE skin_copies (
    copy_id      bigint GENERATED ALWAYS AS IDENTITY (START WITH 1000000) PRIMARY KEY,
    account_id   integer NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    skin_id      integer NOT NULL CHECK (skin_id >= 0),
    paint_color  text,                 -- '#rrggbb' for a colour paint, else NULL
    paint_shader integer,              -- the shader id for a shader paint, else NULL
    painted_at   timestamptz NOT NULL DEFAULT now(),
    CHECK ((paint_color IS NULL) <> (paint_shader IS NULL))
);
CREATE INDEX skin_copies_owner_idx ON skin_copies (account_id, skin_id);

-- Skin delta with audit row (0103), now refusing to take away a painted unit: the unpainted units are
-- count - painted copies. Raises 'Item not owned' when the player does not own enough of those.
CREATE OR REPLACE FUNCTION kn_add_skin(p_id integer, p_skin integer, p_delta integer, p_source text, p_ref text)
RETURNS integer LANGUAGE plpgsql AS $fn$
DECLARE
    v_count integer;
BEGIN
    IF p_delta >= 0 THEN
        INSERT INTO inventory (account_id, skin_id, count) VALUES (p_id, p_skin, p_delta)
        ON CONFLICT (account_id, skin_id) DO UPDATE SET count = inventory.count + p_delta
        RETURNING count INTO v_count;
    ELSE
        UPDATE inventory SET count = count + p_delta
        WHERE account_id = p_id AND skin_id = p_skin AND count + p_delta >= 0
          AND count + p_delta >= (SELECT count(*) FROM skin_copies c
                                  WHERE c.account_id = inventory.account_id AND c.skin_id = inventory.skin_id)
        RETURNING count INTO v_count;
        IF NOT FOUND THEN
            RAISE EXCEPTION USING ERRCODE = 'P0001', MESSAGE = 'Item not owned';
        END IF;
    END IF;
    IF p_delta <> 0 THEN
        INSERT INTO skin_transactions (account_id, skin_id, delta, source, ref) VALUES (p_id, p_skin, p_delta, p_source, p_ref);
    END IF;
    RETURN v_count;
END
$fn$;
