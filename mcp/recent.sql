\pset pager off
SELECT handnumber, betround, action, amount, left(coalesce(note,''),40) note, to_timestamp(ts_ms/1000) ts
FROM bot_nn_decision ORDER BY ts_ms DESC LIMIT 12;
