-- 0105_twofa: Krunker v3.1.1 "2FA" (the menu's 5x5 pattern grid, index.js act2FA /
-- sub2FA / twoFAres) for password logins.  accounts.twofa_secret holds a
-- kn_password_hash of the pattern (sorted letters of config.alphabet).
-- kn_login_lookup gains the twofa column (return type change: drop + create).
-- Owner: svc-accounts.

DROP FUNCTION kn_login_lookup(text, text, integer, integer, integer);

CREATE FUNCTION kn_login_lookup(p_name text, p_ip text, p_throttle_s integer,
    p_block_max integer, p_block_days integer)
RETURNS TABLE (ip_blocked boolean, throttled boolean, account_id integer, password_hash text,
               ban_reason text, ban_expires bigint, banned boolean, twofa text)
LANGUAGE sql STABLE AS $fn$
    SELECT
        p_ip IS NOT NULL AND EXISTS (SELECT 1 FROM login_ip_blocks k
            WHERE k.ip = p_ip AND k.failures > p_block_max AND NOT k.whitelist
              AND k.updated_at > now() - make_interval(days => p_block_days)),
        p_ip IS NOT NULL AND p_throttle_s > 0 AND EXISTS (SELECT 1 FROM login_failures f
            WHERE lower(f.username) = lower(p_name) AND f.ip = p_ip
              AND f.at > now() - make_interval(secs => p_throttle_s)),
        a.id, a.password_hash,
        b.reason, coalesce(extract(epoch FROM b.expires_at)::bigint, 0),
        (b.reason IS NOT NULL OR b.expires_at IS NOT NULL),
        a.twofa_secret
    FROM (SELECT 1) one
    LEFT JOIN accounts a ON lower(a.username) = lower(p_name)
    LEFT JOIN LATERAL kn_active_ban(a.id, p_ip) b ON true
$fn$;
