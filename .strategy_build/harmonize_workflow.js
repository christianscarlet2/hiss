export const meta = {
  name: 'rhapsodic-harmonization',
  description: 'Tier-2 Wave-2 books + Tao Te Ching / Sun Tzu Art of War / 48 Laws of Power, reconcile into the OHF source, then loop-until-clean rhapsodic harmonization. Source-tree only; never deploys/restarts.',
  phases: [
    { title: 'Wave2' },
    { title: 'Wisdom' },
    { title: 'Reconcile' },
    { title: 'Harmonize' },
  ],
}

// ---- shared guardrails every editing agent MUST obey -----------------------
const GUARD = `
HARD RULES (a money-playing bot loads this strategy):
- Edit ONLY files under c:/www/openholdembot_old/.strategy_build/strategy/*.ohf (the SOURCE tree).
  Do NOT touch Release/, do NOT copy anything, do NOT restart Hiss, do NOT deploy.
- OpenPPL grammar: there is NO not-equal operator. NEVER write '<>' or '!=' -> use NOT (a = b).
  Use only engine-native symbols + already-defined f$/list names. New dials must use engine-native
  primitives (bblind sblind tgi_ante tgi_level tgi_players_remaining nplayersdealt nopponentsplaying
  PotSize AmountToCall StackSize EffectiveMaxStacksizeOfActiveOpponents prwin randomround Raises Calls
  InButton InCutOff InEarlyPosition InMiddlePosition InLatePosition InSmallBlind InBigBlind HaveOverPair
  HaveTopPair HaveSet HaveTwoPair FlushPossible StraightPossible) or existing f$ helpers.
- UNITS: PotSize/StackSize/AmountToCall/f$EffStack are all in BIG BLINDS. SPR = f$EffStack/PotSize
  (NO bblind multiply). M is a dimensionless ratio.
- After ANY edit run: cd c:/www/openholdembot_old/.strategy_build && python build_and_lint.py
  Fix every line under [ERRORS] (especially "illegal operator") until it prints "no structural / reference
  errors" and exits 0. The [UNKNOWN IDENTIFIERS] for icm_fold/icm_callwin/icm_calllose/o/s are KNOWN-OK
  false positives, ignore them.
- Keep the citation convention in 00_notes.ohf. Be CONSERVATIVE: only change live behavior to fix a real
  contradiction or to wire a clearly-supported improvement; preserve the [N]/[B]/[T] spine.
`;

const DELTA_SCHEMA = {
  type: 'object',
  additionalProperties: false,
  required: ['source', 'net_new'],
  properties: {
    source: { type: 'string', description: 'tag(s) + one-line thesis per book' },
    net_new: { type: 'string', description: 'ONLY net-new, high-signal deltas vs what is already in the OHF: range/dial/typing changes with page cites, plus any unimplementable->backlog items. Skip anything already covered by [N][B][T][HoH][KE][SSNL][ToP][SM][CtM][PMM][MoH][SNG][EM][TN][HC][PNL][ASH][EG][ER][LTBR][JL][RE][SNF][W][SYS][NSS][HUM][HUSNG][DFR][RF]. Be terse.' },
  },
};

const BOOKDIR = 'C:/Users/scarl/Downloads/PokerBooks';
const READ_GT = `Read these for ground truth first (do not re-derive what they already contain): c:/www/openholdembot_old/.strategy_build/strategy/05_config.ohf, 20_lists.ohf, 10_opponents.ohf, 40_preflop.ohf, 50_flop.ohf, 60_turn.ohf, 70_river.ohf.`;
const METHOD = `Extract each PDF with: pdftotext "<path>" /tmp/<name>.txt (poppler, on PATH). If a file is image-only / yields little, OCR a few key pages with pypdfium2+tesseract. .mobi: try ebook-convert. grep -n -i for the strategy topics; read ~40 lines around strong hits. Do NOT read cover to cover.`;

// =========================== PHASE 1: WAVE 2 ===============================
phase('Wave2')
const wave2Clusters = [
  { label: 'cash-misc', books: [
    `${BOOKDIR}/Poker books 1/2 - (Angel Largay) No-Limit Texas Hold'em - A Complete Course.pdf`,
    `${BOOKDIR}/Poker Books 2/64 - Internet Texas Hold'em Winning Strategies From An Internet Pro (Matthew Hilger).pdf`,
    `${BOOKDIR}/Poker books 1/26 - lee.jones. Winning low limit holdem.pdf`,
    `${BOOKDIR}/Poker books 1/25 - Swaynes Advanced Degree in Hold'em.pdf`,
    `${BOOKDIR}/Poker Books 2/58 - Holdem Brain by King Yao Poker eBook.pdf`,
    `${BOOKDIR}/Poker Books 4/137 - Stoxtrader_Winning_Tough_Holdem_Games - BAD ENGLISH TRANSLATION.pdf`,
  ]},
  { label: 'tournament', books: [
    `${BOOKDIR}/Poker Books 3/106 - Championship No-Limit & Pot-Limit Hold'em (T.J. Cloutier & Tom McEvoy).pdf`,
    `${BOOKDIR}/Poker Books 2/41 - Winning Poker Tournaments - One hand At A Time Vol.1 (Turner, Lynch, Fleet).pdf`,
    `${BOOKDIR}/Poker Books 2/42 - Winning Poker Tournaments One Hand at a Time Vol. 2 (Turner, Lynch, Fleet).pdf`,
    `${BOOKDIR}/Poker Books 3/89 - Stomp The Comp (Michael Vall).pdf`,
    `${BOOKDIR}/Poker Books 4/146 - destination-final-table.pdf`,
    `${BOOKDIR}/Poker Books 4/135 - Tournament Tactics (Roy Rounder).pdf`,
    `${BOOKDIR}/Poker books 1/27 - Sylvester Suzuki - Poker Tournament Strategy.pdf`,
    `${BOOKDIR}/Poker Books 3/91 - Tournament Poker & the Art of War - Apostolico.pdf`,
  ]},
  { label: 'classics', books: [
    `${BOOKDIR}/Poker books 1/15 - Super System 2 - A Course in Power Poker (Doyle Brunson).pdf  (mine only the NLHE parts NOT already in [B])`,
    `${BOOKDIR}/Poker books 1/32 - Play Poker Like the Pros (Phil Hellmuth).pdf  (finish [H])`,
    `${BOOKDIR}/Poker books 1/16 - Phil Gordon's Little Green Book (Phil Gordon).pdf`,
    `${BOOKDIR}/Poker books 1/33 - Phil Gordon Little Gold Book.pdf`,
    `${BOOKDIR}/Poker Books 2/75 - Daniel Negreanu - Holdem Wisdom for all players.pdf`,
    `${BOOKDIR}/Poker Books 2/73 - Decide to Play Great Poker (Annie Duke).mobi`,
  ]},
  { label: 'theory-leaks', books: [
    `${BOOKDIR}/Poker Books 3/95 - Poker Winners Are Different - David Sklansky 2009.mobi`,
    `${BOOKDIR}/Poker Books 3/85 - David Sklansky - The Eight Mistakes In Poker.pdf`,
    `${BOOKDIR}/Poker Books 3/87 - Analytical No-Limit Holdem.pdf`,
    `${BOOKDIR}/Poker Books 2/53 - Elements Of Poker by Tommy Angelo.pdf`,
    `${BOOKDIR}/Poker books 1/20 - The Intelligent Poker Player (Philip Newall).pdf`,
  ]},
  { label: 'math', books: [
    `${BOOKDIR}/Poker books 1/24 - Practical Poker Math (Pat  Dittmar).pdf`,
    `${BOOKDIR}/Poker Books 4/132 - poker_math VT.pdf`,
    `${BOOKDIR}/Poker Books 2/60 - Hole Card Confessions (Owen Gaines).pdf`,
  ]},
  { label: 'pkr-articles', books: [
    `ALL PDFs under "${BOOKDIR}/Poker Books 4/157 - Cash/" (incl. Six Max Cash subfolder) and "${BOOKDIR}/Poker Books 4/158 - Heads Up Articles/". ~70 short PKR strategy articles - skim for net-new bet-sizing/exploit/spot nuggets only.`,
  ]},
  { label: 'caro-lectures', books: [
    `The STRATEGY lectures under "${BOOKDIR}/Poker Books 4/161 - caro lecture/" (position, folding, raising, gear-shifting, image, when-to-quit, pot-odds). SKIP the pure tells/psychology ones (already backlog).`,
  ]},
]
const wave2 = await parallel(wave2Clusters.map(c => () =>
  agent(`Mine these NLHE books for ONLY net-new actionable deltas to fold into an existing, already-rich OpenPPL bot. ${METHOD}\nBOOKS:\n- ${c.books.join('\n- ')}\n${READ_GT}\nReturn terse net-new deltas only (ranges/dials/typing with page cites; unimplementable->backlog). Most of these overlap heavily with what is already integrated - it is fine to return little. Assign a short citation tag per book.`,
    { label: `wave2:${c.label}`, phase: 'Wave2', schema: DELTA_SCHEMA, agentType: 'general-purpose' })
)).then(r => r.filter(Boolean))

// =========================== PHASE 2: WISDOM ===============================
phase('Wisdom')
const wisdom = await parallel([
  () => agent(`From your own knowledge (no files needed), map the TAO TE CHING (Lao Tzu) and SUN TZU'S THE ART OF WAR to an OpenPPL No-Limit Hold'em bot. For EACH applicable principle give: the principle (with chapter/verse where known), the poker translation, and the CONCRETE OHF expression - which EXISTING dial it reinforces (e.g. wu-wei/"do not force" -> pot-control + lower bluff freq + patience/tightness; water/yielding -> adapt to opponent type + fold-and-flow; "know the enemy and yourself" -> opponent typing + stack/M self-awareness; "all warfare is deception" -> uniform value/bluff sizing + balanced randomround frequencies + timing tells; attack weakness/avoid strength -> attack f$Opp_Foldy, never bluff f$Opp_IsStation; "win without fighting" -> steal uncontested pots / fold equity; terrain -> position is power; "do not press a desperate foe" -> do not bluff a pot-committed short stack) OR a small NEW soft dial if warranted. Use citation tags [Tao] and [SunTzu]. Flag principles that belong in the Lilith coach (mcp/poker_coach_hype.py), not the OHF. ${READ_GT}\nReturn the same {source, net_new} structure: net_new = the concrete OHF mappings + new citations to add, terse.`,
    { label: 'wisdom:tao-suntzu', phase: 'Wisdom', schema: DELTA_SCHEMA, agentType: 'general-purpose' }),
  () => agent(`From your own knowledge, map two twinned power-realism texts to an OpenPPL poker bot: (A) the 48 LAWS OF POWER (Robert Greene) and (B) MACHIAVELLI'S THE PRINCE - select only rules with a MACHINE-OBSERVABLE poker translation. 48 Laws likely-applicable: Law 4 (say less than necessary -> never telegraph: uniform bet sizing), Law 15 (crush your enemy totally -> value-bet/stack stations to the felt: f$BetPctVsStation), Law 16 (use absence to command respect -> don't over-bluff / table image), Law 28 (enter with boldness -> aggression + fold equity), Law 35 (mastery of timing -> M/patience + timing tells), Law 48 (assume formlessness -> balanced, unpredictable frequencies via randomround). The Prince likely-applicable: better-to-be-feared-than-loved (a tight-aggressive image earns folds -> fold equity / don't over-bluff to keep respect), the LION AND THE FOX (combine strength=value/aggression with cunning=deception/traps -> balanced value+bluff, trap the maniac), virtu vs fortuna (seize +EV boldly, control variance via ICM/bankroll), adapt to the times (adjust to opponent type + stack depth), economy of cruelty / strike decisively (value to the felt, raise-or-fold over passive calling), avoid being hated (don't spew). For each: principle -> poker translation -> concrete OHF dial it reinforces or a small new dial. Tags [48L] and [Prince]. List principles that do NOT map (interpersonal/meta) as out-of-scope or coach-layer. ${READ_GT}\nReturn {source, net_new}: net_new = concrete OHF mappings + citations, terse.`,
    { label: 'wisdom:48laws-prince', phase: 'Wisdom', schema: DELTA_SCHEMA, agentType: 'general-purpose' }),
]).then(r => r.filter(Boolean))

// =========================== PHASE 3: RECONCILE ============================
phase('Reconcile')
const allDeltas = [...wave2, ...wisdom].map(d => `### ${d.source}\n${d.net_new}`).join('\n\n')
const reconcileLog = await agent(
`You are the single editor reconciling new research into the ScarletBeast OHF. ${GUARD}
Apply the HIGH-CONFIDENCE, well-cited deltas below into the source .ohf files: add citations to 00_notes.ohf;
add/adjust dials in 05_config.ohf; ranges in 20_lists.ohf; typing in 10_opponents.ohf; wire into 40_preflop/
50_flop/60_turn/70_river where clearly warranted. Integrate the [Tao]/[SunTzu]/[48L] mappings as citations on
the dials they reinforce + at most a few small new soft dials. Skip low-confidence / redundant / unimplementable
items (append those to LIMITATIONS.md backlog instead). Edit conservatively. After editing, run build_and_lint.py
and fix until clean. Do NOT deploy.

DELTAS:
${allDeltas}

Return a terse changelog: files touched, dials/ranges/citations added, what you deferred to backlog, and the final
build_and_lint verdict line.`,
  { label: 'reconcile', phase: 'Reconcile', agentType: 'general-purpose' })

// =========================== PHASE 4: HARMONIZE ============================
phase('Harmonize')
const HARM_SCHEMA = {
  type: 'object', additionalProperties: false,
  required: ['clean', 'changes', 'remaining'],
  properties: {
    clean: { type: 'boolean', description: 'true if this round found NO further real issues to fix' },
    changes: { type: 'string', description: 'what you fixed this round (terse)' },
    remaining: { type: 'string', description: 'issues intentionally left (with why), terse' },
  },
}
const dims = [
  'contradictions between dials/streets (e.g. a hand both committed and pot-controlled; overlapping stack-band gates)',
  'redundant or DEAD dials/lists (defined but never referenced) and duplicate logic - consolidate or wire them',
  'threshold coherence across the system (SPR bands, M-zones, push-fold/reshove/short-stack boundaries line up with no gaps/overlaps)',
  'street-to-street coherence (preflop aggressor -> flop cbet -> turn barrel -> river story is consistent; opponent-type gates applied uniformly)',
  'citation & comment integrity (every dial cited; 00_notes lists every source tag; comments match the code)',
  'elegance/rhythm: consistent naming, ordering, sizing ladders, and that value=bluff sizing / balanced frequencies hold throughout',
]
const harmonyLog = []
let round = 0
const MAX_ROUNDS = 8
let dry = 0
while (round < MAX_ROUNDS && dry < 2 && (!budget.total || budget.remaining() > 60000)) {
  const dim = dims[round % dims.length]
  const res = await agent(
`Rhapsodic harmonization pass #${round + 1} of the ScarletBeast OHF. ${GUARD}
Focus THIS round on ONE dimension: ${dim}
Audit the whole strategy (.strategy_build/strategy/*.ohf) for issues in that dimension, FIX the real ones
conservatively (don't invent strategy; bring coherence, rhythm, harmony), then run build_and_lint.py until clean.
If you find nothing genuinely worth changing, make no edits and report clean=true.`,
    { label: `harmonize#${round + 1}:${dim.slice(0, 24)}`, phase: 'Harmonize', schema: HARM_SCHEMA, agentType: 'general-purpose' })
  if (res) {
    harmonyLog.push(`round ${round + 1} [${dim.slice(0, 30)}]: ${res.clean ? 'clean' : res.changes}`)
    dry = res.clean ? dry + 1 : 0
  }
  round++
  log(`harmonize round ${round} done; price so far ~${Math.round(budget.spent() / 1000)}k output tokens`)
}

return {
  wave2_books: wave2.length,
  wisdom_sources: wisdom.length,
  reconcile: reconcileLog,
  harmonize_rounds: round,
  harmonize_log: harmonyLog,
  price_output_tokens: budget.spent(),
  note: 'Source tree edited + lint-clean. NOT deployed - review changelog then mirror to Release/bot_logic/Strategy + regenerate master + restart Hiss.',
}
