//******************************************************************************
//
// This file is part of the OpenHoldem project
//    Licensed under GPL v3: http://www.gnu.org/licenses/gpl.html
//
//******************************************************************************
//
// Purpose: OpenAI advisor + steering bridge  (SCAFFOLD - DISABLED BY DEFAULT)
//
//   A single seam through which the bot can consult OpenAI for a recommendation,
//   be steered from the Terminal console, and propose .ohf improvements. The
//   actual network call (Ask) is intentionally STUBBED: nothing leaves the
//   machine and play is never changed unless the operator explicitly enables it.
//
//   Enable / configure via registry HKCU\Software\ScarletBeast:
//     OpenAiEnabled    (DWORD)  1 = allow live calls (default 0 = stub)
//     OpenAiAutoHijack (DWORD)  1 = auto-consult on no-heuristic-fit spots
//     OpenAiApiKey     (SZ)     the API key (never hard-coded)
//     OpenAiModel      (SZ)     model id (default gpt-4o-mini)
//
//   Console commands (typed into the Terminal prompt, see HandleConsoleCommand):
//     /stop                 pause the autoplayer (openai_paused = 1)
//     /play                 resume
//     /goto <anchor>        steer the decision tree to an .ohf anchor
//     /strategy <name>      steer to a style (smallball|power|hybrid)
//     /hijack               send the current state/context/chat/decisions to
//                           OpenAI for a how-to-continue recommendation
//     /improve <text>       ask OpenAI to propose an .ohf edit from the live
//                           context + decision tree + your instruction
//
//******************************************************************************

#ifndef INC_COPENAIADVISOR_H
#define INC_COPENAIADVISOR_H

#include <afx.h>

// Sentinel returned by the openai_action symbol meaning "no recommendation"
// (so a disabled advisor never changes play).
const double kOpenAiNoOpinion = -999999.0;

class COpenAiAdvisor {
 public:
  // --- configuration (registry-backed) ---
  static bool    Enabled();          // live calls allowed?  (default false)
  static bool    AutoHijackEnabled();
  static CString ApiKey();
  static CString Model();

  // --- steering state (read by CSymbolEngineOpenAI) ---
  static bool   IsPaused()        { return _paused; }
  static int    SteerAnchor()     { return _steer_anchor; }
  static double LastAction()      { return _last_action; }
  static void   ResetSteering();

  // --- console command router. Returns true if it consumed the input. ---
  static bool HandleConsoleCommand(const CString &raw);

  // --- operations (all safe no-ops / log-only while disabled) ---
  static void    Hijack(const CString &reason);                 // manual or auto
  static void    ConsultIfNoFit();                              // auto-hijack hook
  static void    ProposeImprovement(const CString &instruction);

 private:
  static CString BuildContextJson(const CString &purpose, const CString &instruction);
  static CString Ask(const CString &purpose, const CString &instruction);  // STUB seam
  static int     AnchorIdFromName(const CString &name);
  static void    ToDecisions(const CString &text);

 private:
  static bool _paused;
  static int  _steer_anchor;     // 0 = none; see AnchorIdFromName
  static double _last_action;    // last action code OpenAI suggested (kOpenAiNoOpinion = none)
};

#endif  // INC_COPENAIADVISOR_H
