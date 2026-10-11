-- Lobby tuples keep their key order (svc-matchmaker / mm-parity).
-- jsonb normalises object key order, so a lobby read back by another brain listed
-- its meta keys in jsonb order instead of the game server's (v3.1.1 registry.ts
-- passes the node's tuple through unchanged). json stores the text as written.
ALTER TABLE mm_lobbies ALTER COLUMN tuple TYPE json USING tuple::json;
