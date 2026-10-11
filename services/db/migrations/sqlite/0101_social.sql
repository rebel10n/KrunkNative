-- 0101_social (SQLite): follows, clans, chat, feed, mail, reports. Same tables
-- as postgres/0101; the follow counters use triggers like postgres.

CREATE TABLE follows (
    follower_id  INTEGER NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    followee_id  INTEGER NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    created_at   TIMESTAMP NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now')),
    PRIMARY KEY (follower_id, followee_id),
    CHECK (follower_id <> followee_id)
);
CREATE INDEX follows_followee_idx ON follows (followee_id, created_at DESC);

CREATE TRIGGER follows_count_ins AFTER INSERT ON follows BEGIN
    UPDATE players SET player_following = player_following + 1 WHERE player_id = NEW.follower_id;
    UPDATE players SET player_followed  = player_followed  + 1 WHERE player_id = NEW.followee_id;
END;
CREATE TRIGGER follows_count_del AFTER DELETE ON follows BEGIN
    UPDATE players SET player_following = max(player_following - 1, 0) WHERE player_id = OLD.follower_id;
    UPDATE players SET player_followed  = max(player_followed  - 1, 0) WHERE player_id = OLD.followee_id;
END;

CREATE TABLE clans (
    clan_id        INTEGER PRIMARY KEY AUTOINCREMENT,
    clan_name      TEXT NOT NULL,
    clan_playerid  INTEGER REFERENCES accounts(id) ON DELETE SET NULL,
    clan_score     INTEGER NOT NULL DEFAULT 0,
    clan_funds     INTEGER NOT NULL DEFAULT 0 CHECK (clan_funds >= 0),
    clan_rank      INTEGER NOT NULL DEFAULT 0,
    clan_motd      TEXT NOT NULL DEFAULT '',
    clan_discord   TEXT,
    clan_region    INTEGER NOT NULL DEFAULT 0,
    created_at     TIMESTAMP NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now'))
);
CREATE UNIQUE INDEX clans_name_lower_uq ON clans (lower(clan_name));
CREATE INDEX clans_score_idx ON clans (clan_score DESC);

CREATE TABLE clan_members (
    account_id  INTEGER PRIMARY KEY REFERENCES accounts(id) ON DELETE CASCADE,
    clan_id     INTEGER NOT NULL REFERENCES clans(clan_id) ON DELETE CASCADE,
    rank        INTEGER NOT NULL DEFAULT 0,
    score       INTEGER NOT NULL DEFAULT 0,
    kills       INTEGER NOT NULL DEFAULT 0,
    deaths      INTEGER NOT NULL DEFAULT 0,
    joined_at   TIMESTAMP NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now'))
);
CREATE INDEX clan_members_clan_idx ON clan_members (clan_id);

CREATE TABLE clan_requests (
    clan_id     INTEGER NOT NULL REFERENCES clans(clan_id) ON DELETE CASCADE,
    account_id  INTEGER NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    created_at  TIMESTAMP NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now')),
    PRIMARY KEY (clan_id, account_id)
);

CREATE TABLE clan_messages (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    clan_id     INTEGER NOT NULL REFERENCES clans(clan_id) ON DELETE CASCADE,
    sender_id   INTEGER REFERENCES accounts(id) ON DELETE SET NULL,
    body        TEXT NOT NULL,
    created_at  TIMESTAMP NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now'))
);
CREATE INDEX clan_messages_clan_idx ON clan_messages (clan_id, id DESC);

CREATE TABLE chat_messages (
    id           INTEGER PRIMARY KEY AUTOINCREMENT,
    sender_id    INTEGER NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    receiver_id  INTEGER NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    body         TEXT NOT NULL,
    created_at   TIMESTAMP NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now')),
    read_at      TIMESTAMP
);
CREATE INDEX chat_messages_pair_idx ON chat_messages (min(sender_id, receiver_id), max(sender_id, receiver_id), id DESC);
CREATE INDEX chat_messages_receiver_idx ON chat_messages (receiver_id, read_at);
CREATE INDEX chat_messages_sender_idx ON chat_messages (sender_id, created_at);

CREATE TABLE feed_posts (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    author_id   INTEGER NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    body        TEXT NOT NULL,
    type        INTEGER NOT NULL DEFAULT 0,
    link        TEXT,
    link2       TEXT,
    created_at  TIMESTAMP NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now')),
    deleted_at  TIMESTAMP
);
CREATE INDEX feed_posts_author_idx ON feed_posts (author_id, id DESC);
CREATE INDEX feed_posts_recent_idx ON feed_posts (id DESC) WHERE deleted_at IS NULL;

CREATE TABLE feed_comments (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    post_id     INTEGER NOT NULL REFERENCES feed_posts(id) ON DELETE CASCADE,
    author_id   INTEGER NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    body        TEXT NOT NULL,
    created_at  TIMESTAMP NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now'))
);
CREATE INDEX feed_comments_post_idx ON feed_comments (post_id, id);

CREATE TABLE feed_likes (
    post_id     INTEGER NOT NULL REFERENCES feed_posts(id) ON DELETE CASCADE,
    account_id  INTEGER NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    created_at  TIMESTAMP NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now')),
    PRIMARY KEY (post_id, account_id)
);

CREATE TABLE mail (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    account_id  INTEGER NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    sender      TEXT NOT NULL DEFAULT 'Krunker',
    subject     TEXT NOT NULL DEFAULT '',
    body        TEXT NOT NULL DEFAULT '',
    kr          INTEGER NOT NULL DEFAULT 0,
    created_at  TIMESTAMP NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now')),
    read_at     TIMESTAMP
);
CREATE INDEX mail_account_idx ON mail (account_id, id DESC);

CREATE TABLE reports (
    id           INTEGER PRIMARY KEY AUTOINCREMENT,
    reporter_id  INTEGER REFERENCES accounts(id) ON DELETE SET NULL,
    target_id    INTEGER NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    reason       TEXT NOT NULL,
    details      TEXT NOT NULL DEFAULT '',
    created_at   TIMESTAMP NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now')),
    handled_at   TIMESTAMP
);
CREATE INDEX reports_target_idx ON reports (target_id, created_at DESC);

CREATE TABLE moderation_log (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    actor_id    INTEGER REFERENCES accounts(id) ON DELETE SET NULL,
    target_id   INTEGER REFERENCES accounts(id) ON DELETE CASCADE,
    action      TEXT NOT NULL,
    detail      TEXT,
    at          TIMESTAMP NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now'))
);
