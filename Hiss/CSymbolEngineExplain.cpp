//******************************************************************************
//
// This file is part of the OpenHoldem project
//    Licensed under GPL v3: http://www.gnu.org/licenses/gpl.html
//
//******************************************************************************

#include "stdafx.h"
#include "CSymbolEngineExplain.h"

#include <math.h>

#include "CEngineContainer.h"
#include "CFunctionCollection.h"
#include "CFormulaParser.h"
#include "COHScriptObject.h"
#include "ChatTerminalWindow.h"

CSymbolEngineExplain::CSymbolEngineExplain() { _last_tag = ""; }
CSymbolEngineExplain::~CSymbolEngineExplain() {}

void CSymbolEngineExplain::InitOnStartup()   {}
void CSymbolEngineExplain::UpdateOnConnection() { _last_tag = ""; }
void CSymbolEngineExplain::UpdateOnHandreset()  { _last_tag = ""; }
// Re-arm each new betting round and each time it becomes our turn, so a branch
// that fires again on a later street / decision is narrated afresh.
void CSymbolEngineExplain::UpdateOnNewRound()   { _last_tag = ""; }
void CSymbolEngineExplain::UpdateOnMyTurn()     { _last_tag = ""; }
void CSymbolEngineExplain::UpdateOnHeartbeat()  {}

// Format a double for display: integers without a decimal, else 2 places.
static CString FormatNumber(double v) {
  CString s;
  double rounded = (v < 0) ? -floor(-v + 0.5) : floor(v + 0.5);
  if (fabs(v - rounded) < 0.005) s.Format("%.0f", v);
  else                           s.Format("%.2f", v);
  return s;
}

// Replace every {symbol} in the template with that symbol's live value.
CString CSymbolEngineExplain::Interpolate(const CString tmpl) {
  CString out;
  int i = 0, n = tmpl.GetLength();
  while (i < n) {
    TCHAR c = tmpl[i];
    if (c == '{') {
      int close = tmpl.Find('}', i + 1);
      if (close < 0) { out += tmpl.Mid(i); break; }
      CString token = tmpl.Mid(i + 1, close - i - 1);
      token.Trim();
      CString value;
      if (token.IsEmpty()) {
        value = "";
      } else if (p_engine_container != NULL) {
        // Try numeric evaluation first, then a string symbol, else leave the token.
        double d = 0.0;
        if (p_engine_container->EvaluateSymbol(token, &d, false)) {
          value = FormatNumber(d);
        } else {
          CString s;
          if (p_engine_container->EvaluateSymbol(token, &s, false)) value = s;
          else value = "{" + token + "?}";
        }
      } else {
        value = "{" + token + "}";
      }
      out += value;
      i = close + 1;
    } else {
      out += c;
      ++i;
    }
  }
  return out;
}

// Pull the template string out of the f$expl_<tag> function's raw text and fill it.
CString CSymbolEngineExplain::RenderTemplate(const CString tag) {
  CString fname = "f$expl_" + tag;
  CString raw;
  if (p_function_collection != NULL) {
    COHScriptObject *obj = p_function_collection->LookUp(fname);
    if (obj != NULL) raw = obj->function_text();
  }
  if (raw.IsEmpty()) {
    // No template defined: still emit something useful (the tag itself).
    return tag;
  }
  // Extract the text between the first and last double-quote (the string literal),
  // ignoring surrounding comments / whitespace the parser kept in the raw body.
  int first = raw.Find('"');
  int last = raw.ReverseFind('"');
  CString tmpl;
  if (first >= 0 && last > first) tmpl = raw.Mid(first + 1, last - first - 1);
  else { tmpl = raw; tmpl.Trim(); }
  return Interpolate(tmpl);
}

void CSymbolEngineExplain::Emit(const CString tag) {
  // A given tag is narrated at most once per street/turn (re-armed in the
  // UpdateOn* hooks) so live values do not spam the pane every heartbeat.
  if (tag == _last_tag) return;
  _last_tag = tag;
  CString body = RenderTemplate(tag);
  CString line;
  line.Format("\x1b[36m[why]\x1b[0m %s", body.GetString());
  ChatTerminalAppendToScreen("main", kChatTerminalDecisions, line + "\r\n");
}

bool CSymbolEngineExplain::EvaluateSymbol(const CString name, double *result, bool log) {
  // We own every symbol that starts with "explain".
  if (name.Left(7) != "explain") return false;
  // Always evaluates to true so it is transparent at the tail of an AND-condition.
  if (result != NULL) *result = 1.0;
  // Do NOT emit while the parser is verifying symbols (it evaluates each symbol
  // once at parse time); only emit during live decision evaluation.
  if (p_formula_parser != NULL && p_formula_parser->IsParsing()) return true;
  // Tag = the part after "explain_" (or after "explain").
  CString tag;
  if (name.GetLength() > 8 && name[7] == '_') tag = name.Mid(8);
  else                                        tag = name.Mid(7);
  tag.Trim();
  Emit(tag);
  return true;
}

CString CSymbolEngineExplain::SymbolsProvided() {
  // Concrete explain_<tag> names are dynamic and validated via EvaluateSymbol at
  // parse time; "explain" is registered so the prefix is recognised.
  return "explain ";
}
