<?php
/**
 * hiss-log.php — web control for the Hiss advanced-logging toggles.
 * Standalone (no Laravel internals); drop into the hiss.scarletbeast.com public/ dir.
 * Single source of truth = the hiss_log_settings table in the hiss postgres DB.
 *
 * Config via env (set in the vhost / .env): HISS_PG_DSN, HISS_PG_USER, HISS_PG_PASS
 *   e.g. HISS_PG_DSN="pgsql:host=192.168.1.50;port=5432;dbname=hiss"
 * Requires the php-pgsql (pdo_pgsql) extension.  Reachability: the box must be able to
 * reach the hiss postgres (it lives on the Windows bot machine — open it on the LAN or
 * point this at a replica).  See HISS_LOGGING_DAEMON_REQUIREMENTS.md §D.
 */
$DSN  = getenv('HISS_PG_DSN')  ?: 'pgsql:host=127.0.0.1;port=5432;dbname=hiss';
$USER = getenv('HISS_PG_USER') ?: 'postgres';
$PASS = getenv('HISS_PG_PASS') ?: 'dbpass';
$KINDS = ['advanced_logging', 'reporting', 'replays'];

try {
    $db = new PDO($DSN, $USER, $PASS, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);
} catch (Throwable $e) {
    http_response_code(500);
    exit("DB connection failed: " . htmlspecialchars($e->getMessage()));
}

// --- POST: flip a flag (also a JSON API: POST kind+value+identity, Accept: application/json) ---
if ($_SERVER['REQUEST_METHOD'] === 'POST') {
    $kind = $_POST['kind'] ?? '';
    $val  = filter_var($_POST['value'] ?? false, FILTER_VALIDATE_BOOLEAN);
    $ident = preg_replace("/[^A-Za-z0-9_.*-]/", '', $_POST['identity'] ?? '*');
    if (in_array($kind, $KINDS, true) && $ident !== '') {
        $sql = "INSERT INTO hiss_log_settings(identity,$kind,updated_by) VALUES (:i,:v,'web') "
             . "ON CONFLICT (identity) DO UPDATE SET $kind=EXCLUDED.$kind, updated_at=now(), updated_by='web'";
        $st = $db->prepare($sql);
        $st->execute([':i' => $ident, ':v' => $val ? 't' : 'f']);
    }
    if (str_contains($_SERVER['HTTP_ACCEPT'] ?? '', 'application/json')) {
        header('Content-Type: application/json');
        exit(json_encode(['ok' => true]));
    }
    header('Location: ' . strtok($_SERVER['REQUEST_URI'], '?'));
    exit;
}

$rows = $db->query("SELECT identity, advanced_logging, reporting, replays,
    to_char(updated_at,'YYYY-MM-DD HH24:MI') AS updated_at, updated_by
    FROM hiss_log_settings ORDER BY (identity='*') DESC, identity")->fetchAll(PDO::FETCH_ASSOC);
?>
<!doctype html><html><head><meta charset="utf-8"><title>Hiss · advanced logging</title>
<style>
 body{font:15px system-ui;background:#0f1117;color:#e6e6e6;margin:0;padding:24px}
 h1{font-size:20px}.id{color:#7fd1ae}.g{opacity:.6}
 table{border-collapse:collapse;margin-top:12px}th,td{padding:8px 14px;border-bottom:1px solid #2a2f3a;text-align:left}
 .on{color:#3ddc84;font-weight:600}.off{color:#888}
 button{cursor:pointer;background:#1c2330;color:#e6e6e6;border:1px solid #3a4150;border-radius:6px;padding:5px 10px}
 button:hover{border-color:#5a90ff}
 .add{margin-top:16px}input{background:#1c2330;color:#e6e6e6;border:1px solid #3a4150;border-radius:6px;padding:6px}
</style></head><body>
<h1>🔴 Hiss — advanced logging <span class="g">/ reporting / replays</span></h1>
<p class="g"><code>*</code> = global default; a daemon-id row overrides it (each flag falls back to <code>*</code>).</p>
<table><tr><th>identity</th><th>advanced_logging</th><th>reporting</th><th>replays</th><th>updated</th><th>by</th></tr>
<?php foreach ($rows as $r): ?>
 <tr><td class="id"><?=htmlspecialchars($r['identity'])?></td>
 <?php foreach ($KINDS as $k): $on = ($r[$k] === true || $r[$k] === 't' || $r[$k] === '1');
     $isnull = is_null($r[$k]); ?>
   <td><form method="post" style="margin:0">
     <input type="hidden" name="identity" value="<?=htmlspecialchars($r['identity'])?>">
     <input type="hidden" name="kind" value="<?=$k?>">
     <input type="hidden" name="value" value="<?=$on?'0':'1'?>">
     <button class="<?=$on?'on':'off'?>"><?= $isnull ? '∅ inherit' : ($on ? '● ON' : '○ off') ?></button>
   </form></td>
 <?php endforeach; ?>
   <td class="g"><?=htmlspecialchars($r['updated_at'] ?? '')?></td><td class="g"><?=htmlspecialchars($r['updated_by'] ?? '')?></td></tr>
<?php endforeach; ?>
</table>
<form class="add" method="post">
  <input name="identity" placeholder="daemon-id (e.g. swiftsnake-t3)" required>
  <select name="kind"><?php foreach($KINDS as $k) echo "<option>$k</option>";?></select>
  <select name="value"><option value="1">ON</option><option value="0">off</option></select>
  <button>add / set override</button>
</form>
</body></html>
