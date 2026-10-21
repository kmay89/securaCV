-- API tokens are `bo_<id>.<secret>`: the id is the lookup key, the secret is kept as a scrypt hash.
ALTER TABLE api_tokens ADD COLUMN token_id TEXT;
CREATE UNIQUE INDEX IF NOT EXISTS api_tokens_token_id ON api_tokens(token_id);
