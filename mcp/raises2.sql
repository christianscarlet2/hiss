\pset pager off
\pset tuples_only on
\pset format unaligned
\pset fieldsep '|'
SELECT handnumber, action, amount, coalesce(note,''), ts_ms FROM bot_nn_decision
WHERE action IN ('raise','allin') AND ts_ms >  ORDER BY ts_ms ASC LIMIT 20;
