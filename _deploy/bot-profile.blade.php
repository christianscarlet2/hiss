<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="utf-8">
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <title>Scarlet Beast Poker — Simple Bot Profile Creation</title>
    <meta name="description" content="Write a poker bot in OpenPPL: a plain-text rules language. Open a hand-range, decide an action, and send your mind to the felt — no compiler, no SDK.">
    <style>
      :root{--pk-sc:#e10600;--pk-scb:#ff2418;--pk-scs:#ff6a5a;--pk-bone:#ede8df;--pk-dim:#a9a29a;--pk-gold:#d8b25a;
        --pk-mono:'SFMono-Regular',ui-monospace,'JetBrains Mono',Menlo,Consolas,monospace}
      body{margin:0;background:#050404;color:var(--pk-bone);font-family:'Helvetica Neue',Arial,system-ui,sans-serif}
      .wrap{max-width:1000px;margin:0 auto;padding:0 22px}
      h1{font-size:clamp(28px,5vw,50px);text-transform:uppercase;margin:34px 0 8px;letter-spacing:-.01em}
      h1 b{color:var(--pk-scb)}
      h2{margin:44px 0 8px;font-size:22px;text-transform:uppercase;border-bottom:1px solid rgba(255,36,24,.3);padding-bottom:8px}
      h3{margin:26px 0 6px;font-size:16px;text-transform:uppercase;color:var(--pk-bone);letter-spacing:.02em}
      .lead{color:var(--pk-dim);font-size:17px;max-width:760px;line-height:1.6}
      p{color:#cfc7bd;line-height:1.62;max-width:820px}
      ul,ol{color:#cfc7bd;line-height:1.62;max-width:820px}
      a{color:var(--pk-scs)}
      code{font-family:var(--pk-mono);color:var(--pk-gold);font-size:.92em}
      pre{background:#0a0707;border:1px solid rgba(255,255,255,.1);border-radius:10px;padding:16px;overflow:auto;
        font-family:var(--pk-mono);font-size:12.5px;color:#d6e9d6;line-height:1.6}
      pre .c{color:#7d8a7d}    /* comment */
      pre .k{color:var(--pk-scs)} /* keyword */
      pre .a{color:var(--pk-gold)} /* action */
      table.ref{border-collapse:collapse;width:100%;margin:14px 0;font-size:13.5px}
      table.ref th,table.ref td{text-align:left;padding:8px 12px;border-bottom:1px solid rgba(255,255,255,.08);vertical-align:top}
      table.ref th{color:var(--pk-gold);text-transform:uppercase;font-size:11px;letter-spacing:.07em}
      table.ref td code{color:var(--pk-bone)}
      .note{background:#0a0707;border:1px solid rgba(216,178,90,.4);border-radius:12px;padding:14px 18px;margin:18px 0;font-size:14px}
      .note b{color:var(--pk-gold)}
      .panel{background:#0a0707;border:1px solid rgba(255,36,24,.22);border-radius:14px;padding:22px;margin:18px 0}
      .btn{display:inline-flex;align-items:center;gap:8px;background:var(--pk-sc);color:#fff;text-decoration:none;
        font-weight:700;letter-spacing:.04em;text-transform:uppercase;font-size:13px;padding:10px 16px;border-radius:9px}
      .btn:hover{background:var(--pk-scb)}
      .btn.ghost{background:transparent;color:var(--pk-scs);border:1px solid rgba(255,36,24,.45)}
      .toc{display:flex;flex-wrap:wrap;gap:8px;margin:18px 0 6px}
      .toc a{font-family:var(--pk-mono);font-size:11.5px;letter-spacing:.05em;text-transform:uppercase;color:var(--pk-bone);
        border:1px solid rgba(255,36,24,.4);border-radius:6px;padding:5px 10px;text-decoration:none}
      .toc a:hover{background:rgba(255,36,24,.12)}
      .back{display:inline-block;margin:34px 0;font-family:var(--pk-mono);font-size:12px;letter-spacing:.1em;text-transform:uppercase;color:var(--pk-dim);text-decoration:none}
    </style>
</head>
<body>
    @include('partials.chrome')

    @php $mq = ['WRITE A MIND IN PLAIN TEXT','ONE RULE PER LINE · NO COMPILER','OPEN A RANGE · MAKE A READ · STRIKE','THE FELT OBEYS YOUR FORMULA','FOLD NOTHING WORTH PLAYING']; @endphp
    {!! pk_marquee($mq, 'hot', '55') !!}

    @include('partials.subnav', ['active' => 'dev'])

    <div class="wrap">
        <h1>Simple Bot <b>Profile</b> Creation</h1>
        <p class="lead">Your bot's brain is a plain-text file written in <b>OpenPPL</b> — the Open Poker Programming Language. No compiler, no SDK: you write human-readable rules like <code>WHEN InButton AND listOpen RaiseTo 3 FORCE</code>, save a <code>.ohf</code> file, load it, and send a mind to the felt. This guide takes you from an empty file to a thinking, opponent-aware profile.</p>

        <div class="toc">
            <a href="#how">How it plugs in</a>
            <a href="#anatomy">Anatomy of a rule</a>
            <a href="#streets">The four decisions</a>
            <a href="#actions">Actions</a>
            <a href="#symbols">Symbols</a>
            <a href="#lists">Hand ranges</a>
            <a href="#reads">Opponent reads</a>
            <a href="#example">A complete starter</a>
            <a href="#run">Load &amp; run</a>
        </div>

        {{-- ---------------------------------------------------------- how --}}
        <h2 id="how">How a profile plugs into the felt</h2>
        <p>A profile decides one thing each time it's your turn: <b>what action to take</b>. The client evaluates your rules, picks an action (fold / check / call / bet / raise) and a size, and POSTs it to the table:</p>
        <pre>POST /api/v1/tables/{id}/act   →   {"action":"raise","amount":150}</pre>
        <p>You never write that HTTP call — the client does. Your job is the <i>decision</i>. Bet sizes you express in <b>big blinds</b> or <b>pot fractions</b> are converted to the table's chip amount automatically. Opponent stats (VPIP, PFR, aggression…) are pulled live from the table's HUD, so your profile can adapt to who it's playing.</p>

        {{-- ------------------------------------------------------ anatomy --}}
        <h2 id="anatomy">Anatomy of a rule</h2>
        <p>Everything is built from one shape:</p>
        <pre><span class="k">WHEN</span> &lt;condition&gt; &lt;Action&gt; <span class="k">FORCE</span></pre>
        <p>Rules are read top to bottom; the <b>first</b> one whose condition is true fires, and <code>FORCE</code> commits to that action. A profile is a set of named sections (functions and hand lists). Section headers are wrapped in <code>##</code>:</p>
<pre><span class="c">##f$preflop##</span>
<span class="k">WHEN</span> Raises = 0 AND InButton AND listOpenBTN  <span class="a">RaiseTo 3</span>  <span class="k">FORCE</span>
<span class="k">WHEN</span> Raises = 1 AND listPremium               <span class="a">RaiseBy 100%</span> <span class="k">FORCE</span>
<span class="k">WHEN</span> Others                                  <span class="a">Fold</span>        <span class="k">FORCE</span></pre>
        <p>Comments start with <code>//</code>. You can group conditions with <code>[ … ]</code> and combine them with <code>AND</code>, <code>OR</code>, <code>NOT</code>. You can also return a value or another function with <code>RETURN &lt;expr&gt; FORCE</code>.</p>

        {{-- ------------------------------------------------------ streets --}}
        <h2 id="streets">The four decision functions</h2>
        <p>A profile must define four functions — one per betting round. The engine calls the right one for you:</p>
        <table class="ref">
          <tr><th>Function</th><th>When it runs</th></tr>
          <tr><td><code>f$preflop</code></td><td>Before the flop.</td></tr>
          <tr><td><code>f$flop</code></td><td>After the flop.</td></tr>
          <tr><td><code>f$turn</code></td><td>After the turn.</td></tr>
          <tr><td><code>f$river</code></td><td>After the river.</td></tr>
        </table>
        <p>Each one returns an action. A common pattern is to split "no bet to me" from "facing a bet":</p>
<pre><span class="c">##f$flop##</span>
<span class="k">WHEN</span> AmountToCall &gt; 0  <span class="k">RETURN</span> f$flop_vs_bet <span class="k">FORCE</span>
<span class="k">WHEN</span> Others           <span class="k">RETURN</span> f$flop_bet    <span class="k">FORCE</span></pre>

        {{-- ------------------------------------------------------ actions --}}
        <h2 id="actions">Actions you can take</h2>
        <table class="ref">
          <tr><th>Action</th><th>Meaning</th></tr>
          <tr><td><code>Fold</code> / <code>Check</code> / <code>Call</code></td><td>Exactly what they say.</td></tr>
          <tr><td><code>RaiseTo 3</code></td><td>Make the total bet <b>3 big blinds</b> (great for preflop opens).</td></tr>
          <tr><td><code>RaiseBy 100%</code></td><td>Raise <b>by</b> a fraction of the pot (a pot-sized raise).</td></tr>
          <tr><td><code>RaiseHalfPot</code> · <code>RaiseTwoThirdPot</code> · <code>RaiseThreeFourthPot</code> · <code>RaisePot</code></td><td>Bet/raise a pot fraction. When there's no bet yet, these <i>are</i> your bet sizes (c-bets, value bets).</td></tr>
          <tr><td><code>RaiseMax</code></td><td>All-in.</td></tr>
        </table>
        <div class="note"><b>Sizing is automatic.</b> Big-blind and pot-fraction sizes are converted into the table's exact chip amount and clamped to a legal bet, so you never have to compute raw chips.</div>

        {{-- ------------------------------------------------------ symbols --}}
        <h2 id="symbols">The symbols you'll use most</h2>
        <h3>Position</h3>
        <p><code>InEarlyPosition</code> · <code>InMiddlePosition</code> · <code>InCutOff</code> · <code>InButton</code> · <code>InSmallBlind</code> · <code>InBigBlind</code> · <code>InLatePosition</code> · <code>InTheBlinds</code></p>
        <h3>What's happened this round</h3>
        <p><code>Raises</code> · <code>Calls</code> · <code>Opponents</code> · <code>nopponentsplaying</code> — counts of raises/calls before you and players still in.</p>
        <h3>Money (in big blinds)</h3>
        <p><code>StackSize</code> · <code>PotSize</code> · <code>AmountToCall</code> — all expressed in big blinds, so <code>StackSize &lt; 25</code> means "25bb or less".</p>
        <h3>Equity</h3>
        <p><code>prwin</code> — your Monte-Carlo win probability (0…1) against the current opponents. The backbone of good postflop calls: <code>WHEN prwin &gt; 0.55 Call FORCE</code>.</p>
        <h3>Your made hand &amp; draws</h3>
        <p><code>HaveTopPair</code> · <code>HaveOverPair</code> · <code>HaveTwoPair</code> · <code>HaveSet</code> · <code>HaveStraight</code> · <code>HaveFlush</code> · <code>HaveFullHouse</code> · <code>HaveQuads</code> · <code>HaveNuts</code> · <code>HaveFlushDraw</code> · <code>HaveNutFlushDraw</code> · <code>HaveOpenEndedStraightDraw</code> · <code>HaveStraightDraw</code></p>
        <h3>The board</h3>
        <p><code>FlushPossible</code> · <code>StraightPossible</code> · <code>PairOnBoard</code> · <code>Overcards</code></p>

        {{-- ------------------------------------------------------- lists --}}
        <h2 id="lists">Hand ranges</h2>
        <p>Define a range once as a <code>##list…##</code> section, then use its name as a true/false test for "is my hand in this range?". Tokens are <code>AA</code> (pair), <code>AKs</code> (suited), <code>AKo</code> (offsuit):</p>
<pre><span class="c">##listOpenBTN##</span>
22 33 44 55 66 77 88 99 TT JJ QQ KK AA
A2s A3s A4s A5s ATs AJs AQs AKs KTs KJs KQs QJs JTs T9s 98s 87s
ATo AJo AQo AKo KQo

<span class="c">##listPremium##</span>
AA KK QQ AKs AKo</pre>
        <p>Use them in rules: <code>WHEN Raises = 0 AND InButton AND listOpenBTN RaiseTo 3 FORCE</code>.</p>

        {{-- ------------------------------------------------------- reads --}}
        <h2 id="reads">Reading your opponent (live HUD stats)</h2>
        <p>The same stats you see on the table HUD are available to your profile as <code>pt_*</code> symbols, suffixed by <i>which</i> opponent you mean — most often <code>_raischair</code> (the last raiser, i.e. the player you must react to):</p>
        <table class="ref">
          <tr><th>Symbol</th><th>Read</th></tr>
          <tr><td><code>pt_hands_raischair</code></td><td>Sample size — trust the rest only when this is large enough.</td></tr>
          <tr><td><code>pt_vpip_raischair</code></td><td>How loose they play (% of hands entered).</td></tr>
          <tr><td><code>pt_pfr_raischair</code></td><td>How often they raise preflop.</td></tr>
          <tr><td><code>pt_flop_af_raischair</code></td><td>Aggression — low means their bets mean value.</td></tr>
          <tr><td><code>pt_wtsd_raischair</code></td><td>Went-to-showdown — high means a calling station; value-bet thin, never bluff.</td></tr>
        </table>
<pre><span class="c">##f$OppIsNit##</span>  <span class="c">// tight + only trust a real sample</span>
<span class="k">WHEN</span> pt_hands_raischair &gt;= 30 AND pt_vpip_raischair &lt; 18  <span class="k">RETURN</span> true  <span class="k">FORCE</span>
<span class="k">WHEN</span> Others                                            <span class="k">RETURN</span> false <span class="k">FORCE</span>

<span class="c">// ...then 3-bet bluff the nits who fold too much:</span>
<span class="k">WHEN</span> Raises = 1 AND list3betBluff AND f$OppIsNit  <span class="a">RaiseBy 100%</span> <span class="k">FORCE</span></pre>
        <div class="note">If a stat isn't available yet (tiny sample, or the player is unknown), it reads as a neutral default — so gate reads on <code>pt_hands_…</code> and your profile always has a sane fallback.</div>

        {{-- ----------------------------------------------------- example --}}
        <h2 id="example">A complete starter profile</h2>
        <p>Save this as <code>MyBot.ohf</code>. It's tight-aggressive and fully playable — open by position, 3-bet premiums, c-bet, value-bet, and fold the junk.</p>
<pre><span class="c">##f$prwin_number_of_opponents##</span>
nopponentsplaying

<span class="c">##listOpen##</span>
22 33 44 55 66 77 88 99 TT JJ QQ KK AA
A9s ATs AJs AQs AKs KTs KJs KQs QJs JTs T9s 98s 87s
ATo AJo AQo AKo KQo

<span class="c">##listPremium##</span>
AA KK QQ JJ AKs AKo

<span class="c">##f$preflop##</span>
<span class="k">WHEN</span> Raises &gt;= 2 AND listPremium       <span class="a">RaiseBy 100%</span> <span class="k">FORCE</span>
<span class="k">WHEN</span> Raises &gt;= 2                       <span class="a">Fold</span>        <span class="k">FORCE</span>
<span class="k">WHEN</span> Raises = 1 AND listPremium        <span class="a">RaiseBy 100%</span> <span class="k">FORCE</span>
<span class="k">WHEN</span> Raises = 1 AND listOpen           <span class="a">Call</span>        <span class="k">FORCE</span>
<span class="k">WHEN</span> Raises = 1                        <span class="a">Fold</span>        <span class="k">FORCE</span>
<span class="k">WHEN</span> InEarlyPosition AND listPremium   <span class="a">RaiseTo 4</span>   <span class="k">FORCE</span>
<span class="k">WHEN</span> listOpen                          <span class="a">RaiseTo 3</span>   <span class="k">FORCE</span>
<span class="k">WHEN</span> InBigBlind                        <span class="a">Check</span>       <span class="k">FORCE</span>
<span class="k">WHEN</span> Others                            <span class="a">Fold</span>        <span class="k">FORCE</span>

<span class="c">##f$flop##</span>
<span class="k">WHEN</span> AmountToCall = 0 AND (HaveTopPair OR HaveOverPair OR HaveSet OR HaveTwoPair)  <span class="a">RaiseTwoThirdPot</span> <span class="k">FORCE</span>
<span class="k">WHEN</span> AmountToCall = 0 AND nopponentsplaying = 1 AND prwin &gt; 0.40                   <span class="a">RaiseHalfPot</span>     <span class="k">FORCE</span>
<span class="k">WHEN</span> AmountToCall &gt; 0 AND (HaveSet OR HaveTwoPair OR HaveStraight OR HaveFlush)    <span class="a">RaisePot</span>         <span class="k">FORCE</span>
<span class="k">WHEN</span> AmountToCall &gt; 0 AND prwin &gt; 0.55                                            <span class="a">Call</span>             <span class="k">FORCE</span>
<span class="k">WHEN</span> Others <span class="a">Check</span> <span class="k">FORCE</span>

<span class="c">##f$turn##</span>
<span class="k">WHEN</span> AmountToCall = 0 AND (HaveOverPair OR HaveSet OR HaveTwoPair OR HaveStraight OR HaveFlush) <span class="a">RaiseTwoThirdPot</span> <span class="k">FORCE</span>
<span class="k">WHEN</span> AmountToCall &gt; 0 AND prwin &gt; 0.62 <span class="a">Call</span> <span class="k">FORCE</span>
<span class="k">WHEN</span> Others <span class="a">Check</span> <span class="k">FORCE</span>

<span class="c">##f$river##</span>
<span class="k">WHEN</span> AmountToCall = 0 AND (HaveTwoPair OR HaveSet OR HaveStraight OR HaveFlush OR HaveFullHouse) <span class="a">RaiseTwoThirdPot</span> <span class="k">FORCE</span>
<span class="k">WHEN</span> AmountToCall &gt; 0 AND prwin &gt; 0.70 <span class="a">Call</span> <span class="k">FORCE</span>
<span class="k">WHEN</span> Others <span class="a">Check</span> <span class="k">FORCE</span></pre>

        {{-- --------------------------------------------------------- run --}}
        <h2 id="run">Load it &amp; send it to war</h2>
        <ol>
          <li>Save your rules as a <code>.ohf</code> file.</li>
          <li>Open it in the client and point it at <code>poker.scarletbeast.com</code> (set your table id + token).</li>
          <li>Engage the autoplayer. From there it polls the table, evaluates your four functions, and acts for you — adapting to each opponent's HUD as the session unfolds.</li>
        </ol>
        <div class="panel">
          <h3>Want a worked, opponent-aware example?</h3>
          <p>The starter above is deliberately small. A full 6-max profile — position-based open/3-bet/4-bet ranges, c-betting, pot control, semi-bluffing draws, and reads driven by the live HUD — is only a couple hundred lines. Use the API docs for the wire format, then iterate on your ranges.</p>
          <a class="btn" href="/api-docs">Read the API Docs →</a>
          <a class="btn ghost" href="/developers">← Developers Hub</a>
        </div>

        <div class="note" style="border-color:rgba(255,36,24,.4)">
          <b>Three habits of a profile that doesn't go broke:</b>
          <ul style="margin:8px 0 0">
            <li>End every function with a <code>WHEN Others …</code> catch-all so there's always a decision.</li>
            <li>Lean on <code>prwin</code> postflop — it already knows the math you'd otherwise hand-code.</li>
            <li>Gate every <code>pt_*</code> read on <code>pt_hands_… &gt;= 30</code> so a 5-hand sample never moves you.</li>
          </ul>
        </div>

        <a class="back" href="/developers">← Back to the Developers Hub</a>
    </div>

    @php $mq2 = ['WRITE THE MIND · LOAD THE GUN','PRWIN KNOWS · THE FELT REMEMBERS','EVERY RULE A VERDICT','SEND A MACHINE · TAKE A SEAT']; @endphp
    {!! pk_marquee($mq2, 'gold', '50', true) !!}
    @if (function_exists('sb_chrome_footer')) {!! sb_chrome_footer() !!} @endif
    <script src="/js/marquee-engine.js" defer></script>
</body>
</html>
