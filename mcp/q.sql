\pset pager off
SELECT handnumber, betround, action, amount, note, to_timestamp(ts_ms/1000) ts
FROM bot_nn_decision
WHERE handnumber IN ('2783107039','2783109289')
ORDER BY handnumber, ts_ms;
\echo === recent preflop raises/allins ===
SELECT handnumber, betround, action, amount, left(note,90) note, to_timestamp(ts_ms/1000) ts
FROM bot_nn_decision
WHERE betround<=1 AND action IN ('raise','allin')
ORDER BY ts_ms DESC LIMIT 15;
