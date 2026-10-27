-- The promise the store made at checkout, written on the order so every page and mail says the same dates
-- whatever the batch settings are later: the day it ships and the day it should have arrived (YYYY-MM-DD, operator zone).
ALTER TABLE orders ADD COLUMN promised_ship_on TEXT;
ALTER TABLE orders ADD COLUMN promised_arrive_by TEXT;
