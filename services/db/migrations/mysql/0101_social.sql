-- 0101_social (MySQL/MariaDB): follows, clans, chat, feed, mail, reports. Same tables
-- as postgres/0101; the follow counters use triggers like postgres.

CREATE TABLE follows (
    follower_id  BIGINT NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    followee_id  BIGINT NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    created_at   DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    PRIMARY KEY (follower_id, followee_id),
    CHECK (follower_id <> followee_id)
);
CREATE INDEX follows_followee_idx ON follows (followee_id, created_at DESC);

CREATE TRIGGER follows_count_ins AFTER INSERT ON follows FOR EACH ROW BEGIN
    UPDATE players SET player_following = player_following + 1 WHERE player_id = NEW.follower_id;
    UPDATE players SET player_followed  = player_followed  + 1 WHERE player_id = NEW.followee_id;
END;
CREATE TRIGGER follows_count_del AFTER DELETE ON follows FOR EACH ROW BEGIN
    UPDATE players SET player_following = GREATEST(player_following - 1, 0);
    UPDATE players SET player_followed  = GREATEST(player_followed  - 1, 0);
END;

CREATE TABLE clans (
    clan_id        BIGINT AUTO_INCREMENT PRIMARY KEY,
    clan_name      VARCHAR(64) NOT NULL,
    clan_playerid  BIGINT REFERENCES accounts(id) ON DELETE SET NULL,
    clan_score     BIGINT NOT NULL DEFAULT 0,
    clan_funds     BIGINT NOT NULL DEFAULT 0 CHECK (clan_funds >= 0),
    clan_rank      BIGINT NOT NULL DEFAULT 0,
    clan_motd      TEXT NOT NULL DEFAULT (''),
    clan_discord   TEXT,
    clan_region    BIGINT NOT NULL DEFAULT 0,
    created_at     DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3)
);
CREATE UNIQUE INDEX clans_name_lower_uq ON clans (clan_name);
CREATE INDEX clans_score_idx ON clans (clan_score DESC);

CREATE TABLE clan_members (
    account_id  BIGINT PRIMARY KEY REFERENCES accounts(id) ON DELETE CASCADE,
    clan_id     BIGINT NOT NULL REFERENCES clans(clan_id) ON DELETE CASCADE,
    "rank"        BIGINT NOT NULL DEFAULT 0,
    score       BIGINT NOT NULL DEFAULT 0,
    kills       BIGINT NOT NULL DEFAULT 0,
    deaths      BIGINT NOT NULL DEFAULT 0,
    joined_at   DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3)
);
CREATE INDEX clan_members_clan_idx ON clan_members (clan_id);

CREATE TABLE clan_requests (
    clan_id     BIGINT NOT NULL REFERENCES clans(clan_id) ON DELETE CASCADE,
    account_id  BIGINT NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    created_at  DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    PRIMARY KEY (clan_id, account_id)
);

CREATE TABLE clan_messages (
    id          BIGINT AUTO_INCREMENT PRIMARY KEY,
    clan_id     BIGINT NOT NULL REFERENCES clans(clan_id) ON DELETE CASCADE,
    sender_id   BIGINT REFERENCES accounts(id) ON DELETE SET NULL,
    body        TEXT NOT NULL,
    created_at  DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3)
);
CREATE INDEX clan_messages_clan_idx ON clan_messages (clan_id, id DESC);

CREATE TABLE chat_messages (
    id           BIGINT AUTO_INCREMENT PRIMARY KEY,
    sender_id    BIGINT NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    receiver_id  BIGINT NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    body         TEXT NOT NULL,
    created_at   DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    read_at      DATETIME(3)
);
CREATE INDEX chat_messages_pair_idx ON chat_messages (sender_id, receiver_id, id DESC);
CREATE INDEX chat_messages_receiver_idx ON chat_messages (receiver_id, read_at);
CREATE INDEX chat_messages_sender_idx ON chat_messages (sender_id, created_at);

CREATE TABLE feed_posts (
    id          BIGINT AUTO_INCREMENT PRIMARY KEY,
    author_id   BIGINT NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    body        TEXT NOT NULL,
    type        BIGINT NOT NULL DEFAULT 0,
    link        TEXT,
    link2       TEXT,
    created_at  DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    deleted_at  DATETIME(3)
);
CREATE INDEX feed_posts_author_idx ON feed_posts (author_id, id DESC);
CREATE INDEX feed_posts_recent_idx ON feed_posts (id DESC);

CREATE TABLE feed_comments (
    id          BIGINT AUTO_INCREMENT PRIMARY KEY,
    post_id     BIGINT NOT NULL REFERENCES feed_posts(id) ON DELETE CASCADE,
    author_id   BIGINT NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    body        TEXT NOT NULL,
    created_at  DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3)
);
CREATE INDEX feed_comments_post_idx ON feed_comments (post_id, id);

CREATE TABLE feed_likes (
    post_id     BIGINT NOT NULL REFERENCES feed_posts(id) ON DELETE CASCADE,
    account_id  BIGINT NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    created_at  DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    PRIMARY KEY (post_id, account_id)
);

CREATE TABLE mail (
    id          BIGINT AUTO_INCREMENT PRIMARY KEY,
    account_id  BIGINT NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    sender      VARCHAR(64) NOT NULL DEFAULT 'Krunker',
    subject     TEXT NOT NULL DEFAULT (''),
    body        TEXT NOT NULL DEFAULT (''),
    kr          BIGINT NOT NULL DEFAULT 0,
    created_at  DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    read_at     DATETIME(3)
);
CREATE INDEX mail_account_idx ON mail (account_id, id DESC);

CREATE TABLE reports (
    id           BIGINT AUTO_INCREMENT PRIMARY KEY,
    reporter_id  BIGINT REFERENCES accounts(id) ON DELETE SET NULL,
    target_id    BIGINT NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    reason       TEXT NOT NULL,
    details      TEXT NOT NULL DEFAULT (''),
    created_at   DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    handled_at   DATETIME(3)
);
CREATE INDEX reports_target_idx ON reports (target_id, created_at DESC);

CREATE TABLE moderation_log (
    id          BIGINT AUTO_INCREMENT PRIMARY KEY,
    actor_id    BIGINT REFERENCES accounts(id) ON DELETE SET NULL,
    target_id   BIGINT REFERENCES accounts(id) ON DELETE CASCADE,
    action      VARCHAR(32) NOT NULL,
    detail      TEXT,
    at          DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3)
);
