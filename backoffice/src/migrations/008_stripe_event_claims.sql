-- A Stripe event id is claimed before the work and marked applied after it. A
-- claim that never became applied (the process died in between) is taken over
-- by the next redelivery once it is a minute old; a finished event is final.
-- Every event recorded before this column existed was applied in full.
ALTER TABLE stripe_events ADD COLUMN applied_at TEXT;
UPDATE stripe_events SET applied_at = received_at WHERE applied_at IS NULL;
