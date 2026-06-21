//******************************************************************************
//
// This file is part of the OpenHoldem project
//    Licensed under GPL v3: http://www.gnu.org/licenses/gpl.html
//
//******************************************************************************

#include "stdafx.h"
#include "CSymbolEngineOpenAI.h"

#include "COpenAiAdvisor.h"
#include "CFormulaParser.h"

CSymbolEngineOpenAI::CSymbolEngineOpenAI() {}
CSymbolEngineOpenAI::~CSymbolEngineOpenAI() {}

void CSymbolEngineOpenAI::InitOnStartup()    {}
void CSymbolEngineOpenAI::UpdateOnConnection(){}
void CSymbolEngineOpenAI::UpdateOnHandreset() {}
void CSymbolEngineOpenAI::UpdateOnNewRound()  {}
void CSymbolEngineOpenAI::UpdateOnMyTurn()    {}
void CSymbolEngineOpenAI::UpdateOnHeartbeat() {}

bool CSymbolEngineOpenAI::EvaluateSymbol(const CString name, double *result, bool log) {
  if (name.Left(6) != "openai") return false;
  if (name == "openai_paused") {
    if (result != NULL) *result = COpenAiAdvisor::IsPaused() ? 1.0 : 0.0;
    return true;
  }
  if (name == "openai_steer_anchor") {
    if (result != NULL) *result = (double)COpenAiAdvisor::SteerAnchor();
    return true;
  }
  if (name == "openai_action") {
    if (result != NULL) *result = COpenAiAdvisor::LastAction();   // kOpenAiNoOpinion when none
    return true;
  }
  if (name == "openai_autohijack") {
    if (result != NULL) *result = COpenAiAdvisor::AutoHijackEnabled() ? 1.0 : 0.0;
    return true;
  }
  if (name == "openai_consult") {
    // Trigger symbol (transparent true), used at the tail of a no-fit branch.
    if (result != NULL) *result = 1.0;
    // Do not consult while the parser is verifying symbols.
    if (p_formula_parser != NULL && p_formula_parser->IsParsing()) return true;
    COpenAiAdvisor::ConsultIfNoFit();
    return true;
  }
  // Synapse-harmonizer runtime knobs (0..1, 0.5 = neutral), pushed live via /api/knob and read
  // by the OHF (05_config f$OpenRangeKnob / f$AggroFreqMult / f$BluffFreqMult). The "openai_"
  // namespace keeps them in the steering engine; they retune play with no rebuild. [Emrald]
  if (name == "openai_knob_openrange") {
    extern double g_knob_openrange;
    if (result != NULL) *result = g_knob_openrange;
    return true;
  }
  if (name == "openai_knob_aggro") {
    extern double g_knob_aggro;
    if (result != NULL) *result = g_knob_aggro;
    return true;
  }
  if (name == "openai_knob_bluff") {
    extern double g_knob_bluff;
    if (result != NULL) *result = g_knob_bluff;
    return true;
  }
  return false;
}

CString CSymbolEngineOpenAI::SymbolsProvided() {
  return "openai_paused openai_steer_anchor openai_action openai_autohijack openai_consult "
         "openai_knob_openrange openai_knob_aggro openai_knob_bluff ";
}
