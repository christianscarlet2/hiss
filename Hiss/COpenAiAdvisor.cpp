//******************************************************************************
//
// This file is part of the OpenHoldem project
//    Licensed under GPL v3: http://www.gnu.org/licenses/gpl.html
//
//******************************************************************************

#include "stdafx.h"
#include "COpenAiAdvisor.h"

#include "CEngineContainer.h"
#include "ChatTerminalWindow.h"
#include "..\DLLs\Files_DLL\Files.h"

bool   COpenAiAdvisor::_paused       = false;
int    COpenAiAdvisor::_steer_anchor = 0;
double COpenAiAdvisor::_last_action  = kOpenAiNoOpinion;

// ---------------------------------------------------------------------------
// registry helpers (HKCU\Software\ScarletBeast)
// ---------------------------------------------------------------------------

static DWORD ReadRegDword(const char *value, DWORD def) {
  HKEY k; DWORD out = def, sz = sizeof(DWORD), type = 0;
  if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\ScarletBeast", 0, KEY_READ, &k) == ERROR_SUCCESS) {
    RegQueryValueExA(k, value, NULL, &type, (LPBYTE)&out, &sz);
    RegCloseKey(k);
  }
  return out;
}

static CString ReadRegString(const char *value) {
  HKEY k; char buf[1024] = {0}; DWORD sz = sizeof(buf) - 1, type = 0;
  CString out;
  if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\ScarletBeast", 0, KEY_READ, &k) == ERROR_SUCCESS) {
    if (RegQueryValueExA(k, value, NULL, &type, (LPBYTE)buf, &sz) == ERROR_SUCCESS) out = buf;
    RegCloseKey(k);
  }
  return out;
}

bool    COpenAiAdvisor::Enabled()          { return ReadRegDword("OpenAiEnabled", 0) != 0; }
bool    COpenAiAdvisor::AutoHijackEnabled(){ return ReadRegDword("OpenAiAutoHijack", 0) != 0; }
CString COpenAiAdvisor::ApiKey()           { return ReadRegString("OpenAiApiKey"); }
CString COpenAiAdvisor::Model() {
  CString m = ReadRegString("OpenAiModel");
  return m.IsEmpty() ? CString("gpt-4o-mini") : m;
}

void COpenAiAdvisor::ResetSteering() { _steer_anchor = 0; }

void COpenAiAdvisor::ToDecisions(const CString &text) {
  ChatTerminalAppendToScreen("main", kChatTerminalDecisions, text + "\r\n");
}

// ---------------------------------------------------------------------------
// context assembly  ("determine what you need from OpenAI")
// ---------------------------------------------------------------------------

static void AppendSym(CString *json, const char *key, const char *symbol) {
  if (p_engine_container == NULL) return;
  double v = 0.0;
  if (p_engine_container->EvaluateSymbol(CString(symbol), &v, false)) {
    CString one; one.Format("    \"%s\": %.4g,\n", key, v);
    *json += one;
  }
}

CString COpenAiAdvisor::BuildContextJson(const CString &purpose, const CString &instruction) {
  CString json = "{\n";
  CString p; p.Format("  \"purpose\": \"%s\",\n", purpose.GetString()); json += p;
  if (!instruction.IsEmpty()) {
    CString in; in.Format("  \"instruction\": \"%s\",\n", instruction.GetString()); json += in;
  }
  // Game state: the symbols an advisor needs to reason about the spot.
  json += "  \"state\": {\n";
  AppendSym(&json, "betround",       "betround");
  AppendSym(&json, "pot",            "PotSize");
  AppendSym(&json, "amount_to_call", "AmountToCall");
  AppendSym(&json, "eff_stack_bb",   "f$EffStack");
  AppendSym(&json, "opponents",      "nopponentsplaying");
  AppendSym(&json, "raises",         "Raises");
  AppendSym(&json, "prwin",          "prwin");
  AppendSym(&json, "opp_vpip",       "f$Opp_VPIP");
  AppendSym(&json, "opp_pfr",        "f$Opp_PFR");
  AppendSym(&json, "opp_af",         "f$Opp_AF");
  AppendSym(&json, "raiser_timing",  "lastraiseractiontime");
  json += "    \"_\": 0\n  },\n";
  // The four Terminal panes: context / state / decisions / chat.
  CString sections[kChatTerminalSectionCount];
  CString pinned;
  TerminalBrowserGetSnapshot(sections, &pinned);
  const char *names[kChatTerminalSectionCount] = { "context", "state", "decisions", "chat" };
  json += "  \"panes\": {\n";
  for (int i = 0; i < kChatTerminalSectionCount; ++i) {
    CString s = sections[i];
    s.Replace("\\", "\\\\"); s.Replace("\"", "\\\""); s.Replace("\r", " "); s.Replace("\n", " ");
    if (s.GetLength() > 1200) s = s.Left(1200);
    CString one; one.Format("    \"%s\": \"%s\"%s\n", names[i], s.GetString(),
                            (i < kChatTerminalSectionCount - 1) ? "," : "");
    json += one;
  }
  json += "  }\n}";
  return json;
}

// ---------------------------------------------------------------------------
// the network seam (STUB)
// ---------------------------------------------------------------------------

CString COpenAiAdvisor::Ask(const CString &purpose, const CString &instruction) {
  CString context = BuildContextJson(purpose, instruction);
  if (!Enabled()) {
    ToDecisions("\x1b[33m[openai:disabled]\x1b[0m would send "
                + purpose + " request (set HKCU\\Software\\ScarletBeast\\OpenAiEnabled=1 to enable).");
    return "";
  }
  if (ApiKey().IsEmpty()) {
    ToDecisions("\x1b[31m[openai]\x1b[0m enabled but no OpenAiApiKey set in registry; aborting.");
    return "";
  }
  // ===========================================================================
  // TODO (not implemented yet, per design): perform the actual HTTPS POST to
  //   https://api.openai.com/v1/chat/completions
  // with Model(), the ApiKey() bearer token, and `context` as the user message,
  // on a BACKGROUND thread (never the heartbeat thread - see the Scarlet-Beast
  // heartbeat-stall lesson), then parse the JSON response. Until then we return
  // empty so the bot keeps using its own heuristics.
  // ===========================================================================
  CString note;
  note.Format("\x1b[33m[openai]\x1b[0m live call not implemented yet; context assembled (%d bytes).",
              context.GetLength());
  ToDecisions(note);
  return "";
}

// ---------------------------------------------------------------------------
// operations
// ---------------------------------------------------------------------------

void COpenAiAdvisor::Hijack(const CString &reason) {
  ToDecisions("\x1b[35m[hijack]\x1b[0m " + reason);
  CString answer = Ask("hijack", reason);
  if (!answer.IsEmpty()) ToDecisions("\x1b[36m[openai]\x1b[0m " + answer);
}

void COpenAiAdvisor::ConsultIfNoFit() {
  // Called from the .ohf at a no-good-heuristic-fit branch (openai_consult).
  if (!AutoHijackEnabled()) return;
  Hijack("auto: no good heuristic fit for this spot");
}

void COpenAiAdvisor::ProposeImprovement(const CString &instruction) {
  ToDecisions("\x1b[35m[improve]\x1b[0m " + instruction);
  CString answer = Ask("improve_ohf", instruction);
  // Safety: NEVER overwrite the live strategy. Write any proposal to a side file
  // for the operator to review and apply manually.
  CString dir = StrategyDirectory() + "proposed";
  CreateDirectory(dir, NULL);
  CString notice;
  notice.Format("proposal would be written to %s\\ (review before applying).", dir.GetString());
  ToDecisions("\x1b[33m[improve]\x1b[0m " + notice);
}

// ---------------------------------------------------------------------------
// console command routing
// ---------------------------------------------------------------------------

int COpenAiAdvisor::AnchorIdFromName(const CString &name) {
  CString n = name; n.MakeLower(); n.Trim();
  // Keep in sync with the @anchor ids in the .ohf (16_steering.ohf).
  if (n == "preflop")  return 1;
  if (n == "flop")     return 2;
  if (n == "turn")     return 3;
  if (n == "river")    return 4;
  if (n == "smallball") return 11;
  if (n == "power")     return 12;
  if (n == "hybrid")    return 13;
  return 0;
}

bool COpenAiAdvisor::HandleConsoleCommand(const CString &raw) {
  CString cmd = raw; cmd.Trim();
  if (cmd.IsEmpty() || cmd[0] != '/') return false;   // not a steering command
  CString verb = cmd, arg = "";
  int sp = cmd.Find(' ');
  if (sp > 0) { verb = cmd.Left(sp); arg = cmd.Mid(sp + 1); arg.Trim(); }
  verb.MakeLower();

  if (verb == "/stop") {
    _paused = true;  ToDecisions("\x1b[31m[steer]\x1b[0m PAUSED - autoplayer held (openai_paused=1).");
    return true;
  }
  if (verb == "/play") {
    _paused = false; ToDecisions("\x1b[32m[steer]\x1b[0m RESUMED.");
    return true;
  }
  if (verb == "/goto") {
    int id = AnchorIdFromName(arg);
    if (id == 0) { ToDecisions("\x1b[31m[steer]\x1b[0m unknown anchor: " + arg); return true; }
    _steer_anchor = id;
    ToDecisions("\x1b[32m[steer]\x1b[0m steering to anchor '" + arg + "'.");
    return true;
  }
  if (verb == "/strategy") {
    int id = AnchorIdFromName(arg);
    if (id == 0) {
      // Natural-language style request -> let OpenAI map it (stubbed for now).
      Ask("steer_strategy", arg);
    } else {
      _steer_anchor = id;
      ToDecisions("\x1b[32m[steer]\x1b[0m strategy -> '" + arg + "'.");
    }
    return true;
  }
  if (verb == "/hijack") {
    Hijack(arg.IsEmpty() ? CString("manual hijack") : arg);
    return true;
  }
  if (verb == "/improve") {
    if (arg.IsEmpty()) { ToDecisions("\x1b[31m[improve]\x1b[0m usage: /improve <what to change>"); return true; }
    ProposeImprovement(arg);
    return true;
  }
  ToDecisions("\x1b[33m[steer]\x1b[0m unknown command: " + verb
              + "  (/stop /play /goto /strategy /hijack /improve)");
  return true;
}
