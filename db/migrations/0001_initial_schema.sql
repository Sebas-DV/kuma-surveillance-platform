CREATE TYPE user_role AS ENUM ('admin', 'operator', 'viewer');

CREATE TYPE api_key_kind AS ENUM ('session', 'service');

CREATE TABLE devices (
    id uuid PRIMARY KEY DEFAULT gen_random_uuid(),
    name text NOT NULL,
    registration_id text NOT NULL,
    dvr_username text NOT NULL,
    dvr_password_encrypted bytea NOT NULL,
    serial_number text,
    channel_count integer NOT NULL DEFAULT 0,
    enabled boolean NOT NULL DEFAULT true,
    last_seen_at timestamptz,
    created_at timestamptz NOT NULL DEFAULT now(),
    updated_at timestamptz NOT NULL DEFAULT now(),
    CONSTRAINT devices_name_not_blank CHECK (length(btrim(name)) BETWEEN 1 AND 120),
    CONSTRAINT devices_registration_id_not_blank CHECK (length(btrim(registration_id)) BETWEEN 1 AND 64),
    CONSTRAINT devices_username_not_blank CHECK (length(btrim(dvr_username)) BETWEEN 1 AND 64),
    CONSTRAINT devices_channel_count_range CHECK (channel_count BETWEEN 0 AND 512)
);

CREATE UNIQUE INDEX devices_registration_id_key ON devices (registration_id);
CREATE UNIQUE INDEX devices_name_key ON devices (lower(name));

CREATE TABLE device_channels (
    device_id uuid NOT NULL REFERENCES devices (id) ON DELETE CASCADE,
    channel_index integer NOT NULL,
    name text NOT NULL,
    enabled boolean NOT NULL DEFAULT true,
    PRIMARY KEY (device_id, channel_index),
    CONSTRAINT device_channels_index_range CHECK (channel_index BETWEEN 0 AND 511),
    CONSTRAINT device_channels_name_not_blank CHECK (length(btrim(name)) BETWEEN 1 AND 120)
);

CREATE TABLE users (
    id uuid PRIMARY KEY DEFAULT gen_random_uuid(),
    username text NOT NULL,
    password_hash text NOT NULL,
    role user_role NOT NULL DEFAULT 'viewer',
    disabled boolean NOT NULL DEFAULT false,
    failed_logins integer NOT NULL DEFAULT 0,
    locked_until timestamptz,
    created_at timestamptz NOT NULL DEFAULT now(),
    updated_at timestamptz NOT NULL DEFAULT now(),
    CONSTRAINT users_username_format CHECK (username ~ '^[A-Za-z0-9._@-]{3,64}$')
);

CREATE UNIQUE INDEX users_username_key ON users (lower(username));

CREATE TABLE api_keys (
    id uuid PRIMARY KEY DEFAULT gen_random_uuid(),
    user_id uuid NOT NULL REFERENCES users (id) ON DELETE CASCADE,
    name text NOT NULL,
    kind api_key_kind NOT NULL DEFAULT 'service',
    key_hash bytea NOT NULL,
    expires_at timestamptz,
    revoked_at timestamptz,
    last_used_at timestamptz,
    created_at timestamptz NOT NULL DEFAULT now(),
    CONSTRAINT api_keys_name_not_blank CHECK (length(btrim(name)) BETWEEN 1 AND 120),
    CONSTRAINT api_keys_hash_length CHECK (octet_length(key_hash) = 32)
);

CREATE UNIQUE INDEX api_keys_key_hash_key ON api_keys (key_hash);
CREATE INDEX api_keys_user_id_idx ON api_keys (user_id);

CREATE TABLE video_tokens (
    id uuid PRIMARY KEY DEFAULT gen_random_uuid(),
    token_hash bytea NOT NULL,
    user_id uuid NOT NULL REFERENCES users (id) ON DELETE CASCADE,
    device_id uuid NOT NULL REFERENCES devices (id) ON DELETE CASCADE,
    channel_index integer NOT NULL,
    profile smallint NOT NULL DEFAULT 0,
    expires_at timestamptz NOT NULL,
    consumed_at timestamptz,
    created_at timestamptz NOT NULL DEFAULT now(),
    CONSTRAINT video_tokens_hash_length CHECK (octet_length(token_hash) = 32),
    CONSTRAINT video_tokens_channel_range CHECK (channel_index BETWEEN 0 AND 511),
    CONSTRAINT video_tokens_profile_range CHECK (profile BETWEEN 0 AND 3),
    CONSTRAINT video_tokens_expiry_after_creation CHECK (expires_at > created_at)
);

CREATE UNIQUE INDEX video_tokens_token_hash_key ON video_tokens (token_hash);
CREATE INDEX video_tokens_expires_at_idx ON video_tokens (expires_at);
CREATE INDEX video_tokens_device_id_idx ON video_tokens (device_id);
