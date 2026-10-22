-- 0352_skin_copies (MySQL): owned copies that carry a paint (v10.0.3's owned entries: id = the copy's id,
-- m = {paint, paintShader}; doc/v10/paint-copy.md). inventory.count still counts every owned unit, painted or
-- not, so ownership checks are unchanged; a row here is one painted unit of (account, skin). db_add_skin
-- refuses to take away a painted unit (only the unpainted ones are sold, listed, traded or crafted). The copy
-- id starts at 1000000: a stack's sid on the market rows is its store index (below 1000000), so the two never
-- meet. Additive only.
CREATE TABLE skin_copies (
    copy_id      BIGINT NOT NULL AUTO_INCREMENT PRIMARY KEY,
    account_id   BIGINT NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    skin_id      BIGINT NOT NULL CHECK (skin_id >= 0),
    paint_color  VARCHAR(16) NULL,     -- '#rrggbb' for a colour paint, else NULL
    paint_shader INT NULL,             -- the shader id for a shader paint, else NULL
    painted_at   DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    CHECK ((paint_color IS NULL) <> (paint_shader IS NULL))
) AUTO_INCREMENT = 1000000;
CREATE INDEX skin_copies_owner_idx ON skin_copies (account_id, skin_id);
