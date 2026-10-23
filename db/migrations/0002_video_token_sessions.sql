ALTER TABLE video_tokens ADD COLUMN consumed_address text;

ALTER TABLE video_tokens ADD CONSTRAINT video_tokens_consumption_consistent
    CHECK ((consumed_at IS NULL) = (consumed_address IS NULL)) NOT VALID;
