#!/usr/bin/env python3
# Add a token-friendly JSON HUD import so Hiss (Bearer token) can load a .pt4hud
# without multipart: POST /api/v1/hud/import { name?, content_b64 }.
import io
base = "/var/www/poker.scarletbeast.com"

# 1) Controller method (insert before the table() live-HUD method).
ctrl = base + "/app/Http/Controllers/HudController.php"
s = io.open(ctrl, encoding="utf-8").read()
method = (
    "    /** Token-friendly import: a .pt4hud sent as base64 JSON (used by Hiss). */\n"
    "    public function import(Request $request)\n"
    "    {\n"
    "        $data = $request->validate([\n"
    "            'content_b64' => ['required', 'string'],\n"
    "            'name' => ['nullable', 'string', 'max:120'],\n"
    "        ]);\n"
    "        $raw = base64_decode($data['content_b64'], true);\n"
    "        if ($raw === false) {\n"
    "            return response()->json(['error' => 'Invalid base64 content.'], 422);\n"
    "        }\n"
    "        try {\n"
    "            $parsed = Pt4Hud::parse($raw);\n"
    "        } catch (\\Throwable $e) {\n"
    "            return response()->json(['error' => $e->getMessage()], 422);\n"
    "        }\n"
    "        $profile = HudProfile::create([\n"
    "            'user_id' => $request->user()->id,\n"
    "            'name' => $data['name'] ?: $parsed['name'],\n"
    "            'source' => ($data['name'] ?: $parsed['name']) . '.pt4hud',\n"
    "            'rows' => $parsed['rows'],\n"
    "        ]);\n"
    "        $request->user()->update(['hud_profile_id' => $profile->id]);\n"
    "        return response()->json(['ok' => true, 'profile' => ['id' => $profile->id, 'name' => $profile->name]]);\n"
    "    }\n\n")
anchor = "    /**\n     * Live HUD payload for a felt"
if "function import" in s:
    print("controller: already has import()")
elif anchor in s:
    s = s.replace(anchor, method + anchor, 1)
    io.open(ctrl, "w", encoding="utf-8").write(s)
    print("controller: import() added")
else:
    print("controller: ANCHOR NOT FOUND")

# 2) Route under the bot-token group, beside the table HUD route.
rt = base + "/routes/api.php"
r = io.open(rt, encoding="utf-8").read()
ranchor = "        Route::get('/tables/{table}/hud', [\\App\\Http\\Controllers\\HudController::class, 'table']);\n"
radd = "        Route::post('/hud/import', [\\App\\Http\\Controllers\\HudController::class, 'import']);\n"
if "hud/import" in r:
    print("route: already present")
elif ranchor in r:
    r = r.replace(ranchor, ranchor + radd, 1)
    io.open(rt, "w", encoding="utf-8").write(r)
    print("route: added")
else:
    print("route: ANCHOR NOT FOUND")
print("DONE")
