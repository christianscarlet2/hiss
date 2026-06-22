var useEffect = React.useEffect;
var useState = React.useState;

// Seats are spread evenly around the felt ellipse (chair 0 at the bottom centre, then
// distributed by equal angle). cx/cy = .5/.49, semi-axes .45 x .38.
var pc = [
  [], [],
  [[.5,.85],[.5,.13]],
  [[.5,.85],[.136,.31],[.864,.31]],
  [[.5,.85],[.08,.49],[.5,.13],[.92,.49]],
  [[.5,.85],[.101,.601],[.253,.199],[.747,.199],[.899,.601]],
  [[.5,.85],[.136,.67],[.136,.31],[.5,.13],[.864,.31],[.864,.67]],
  [[.5,.85],[.172,.714],[.091,.41],[.318,.166],[.682,.166],[.909,.41],[.828,.714]],
  [[.5,.85],[.203,.745],[.08,.49],[.203,.235],[.5,.13],[.797,.235],[.92,.49],[.797,.745]],
  [[.5,.85],[.23,.766],[.086,.553],[.136,.31],[.356,.152],[.644,.152],[.864,.31],[.914,.553],[.77,.766]],
  [[.5,.85],[.253,.781],[.101,.601],[.101,.379],[.253,.199],[.5,.13],[.747,.199],[.899,.379],[.899,.601],[.747,.781]]
];

// The hero (the human/this player) is chair 3; seat it at the bottom centre and keep the
// other chairs in their rotational order around the ellipse.
var HERO_CHAIR = 3;
function seatPos(nchairs, chair) {
  var ring = pc[nchairs];
  if (!ring || !ring.length) return [.5, .5];
  var idx = (((chair - HERO_CHAIR) % nchairs) + nchairs) % nchairs;
  return ring[idx] || [.5, .5];
}

function e(type, props) {
  var args = [type, props || {}];
  for (var i = 2; i < arguments.length; i += 1) {
    args.push(arguments[i]);
  }
  return React.createElement.apply(React, args);
}

// Trimmed number: up to 2 decimals with trailing zeros (and a bare dot) removed,
// e.g. 100 -> "100", 12.50 -> "12.5", 8.00 -> "8".
function money(value) {
  value = Number(value || 0);
  return value.toFixed(2).replace(/\.?0+$/, '');
}

// Current money display unit, set by App each render. In "usd" mode, values (which the
// bot reports in big blinds) are multiplied by the big-blind size to get a dollar amount.
var DISPLAY = { unit: 'bb', bb: 0 };

// Format a big-blind value for display: "12.5 BB", or "$25" when the dollar unit is active
// (double-click any balance/pot/bet to toggle). Trailing zeros are trimmed either way.
function bb(value) {
  value = Number(value || 0);
  if (DISPLAY.unit === 'usd') {
    return '$' + money(value * DISPLAY.bb);
  }
  return money(value) + ' BB';
}

function thousands(value) {
  value = Math.max(0, Math.round(Number(value || 0)));
  return String(value).replace(/\B(?=(\d{3})+(?!\d))/g, ',');
}

// Three casino chip colours used for every bet (red / green / black).
var CHIP_COLORS = [
  { body: '#cf2233', edge: '#7c0e1a', spot: '#f6eef0', face: '#9c1020' },  // 0 red
  { body: '#1f9a52', edge: '#0e5630', spot: '#eef7f1', face: '#0f6435' },  // 1 green
  { body: '#23262c', edge: '#000000', spot: '#d6c0c4', face: '#0a0a0c' }   // 2 black
];

// Chips for a bet: a taller stack for bigger bets (sqrt-scaled, capped), coloured in three
// value bands (small = red, then green, then black for large bets). Returned bottom-first.
function chipsForBet(bet) {
  var v = Math.max(0.01, Number(bet) || 0);
  var count = Math.min(14, Math.max(1, Math.round(Math.sqrt(v) * 1.5)));
  var stack = [];
  for (var i = 0; i < count; i++) {
    var frac = count > 1 ? i / (count - 1) : 0;   // 0 bottom .. 1 top
    var ci;
    if (v >= 25) ci = frac < 0.34 ? 2 : (frac < 0.7 ? 1 : 0);
    else if (v >= 5) ci = frac < 0.5 ? 1 : 0;
    else ci = 0;
    stack.push(ci);
  }
  return stack;
}

// An angled (perspective) stack of poker chips on the felt; its height grows with the bet.
function chipStackSvg(bet) {
  var stack = chipsForBet(bet);
  var n = stack.length;
  var rx = 27, ry = 9, th = 5, step = 7, W = 66, padB = 10, padT = 9;
  var H = padT + (n - 1) * step + 2 * ry + th + padB;
  var cx = W / 2;
  var bottomCy = H - padB - ry - th;
  var s = '<svg viewBox="0 0 ' + W + ' ' + H + '" xmlns="http://www.w3.org/2000/svg">';
  s += '<defs><filter id="cstk" x="-30%" y="-12%" width="160%" height="135%"><feDropShadow dx="0" dy="2" stdDeviation="1.7" flood-color="#000" flood-opacity=".55"/></filter></defs>';
  s += '<g filter="url(#cstk)">';
  for (var i = 0; i < n; i++) {
    var c = CHIP_COLORS[stack[i]];
    var cy = bottomCy - i * step;
    s += '<ellipse cx="' + cx + '" cy="' + (cy + th) + '" rx="' + rx + '" ry="' + ry + '" fill="' + c.edge + '"/>';
    s += '<ellipse cx="' + cx + '" cy="' + cy + '" rx="' + rx + '" ry="' + ry + '" fill="' + c.body + '"/>';
  }
  s += '</g>';
  var tc = CHIP_COLORS[stack[n - 1]];
  var tcy = bottomCy - (n - 1) * step;
  s += '<ellipse cx="' + cx + '" cy="' + tcy + '" rx="' + (rx - 3) + '" ry="' + (ry - 1.5) + '" fill="none" stroke="' + tc.spot + '" stroke-width="3.6" stroke-dasharray="3 7.4"/>';
  s += '<ellipse cx="' + cx + '" cy="' + tcy + '" rx="' + (rx - 9) + '" ry="' + (ry - 3.5) + '" fill="' + tc.body + '"/>';
  s += '<ellipse cx="' + cx + '" cy="' + tcy + '" rx="' + (rx - 9) + '" ry="' + (ry - 3.5) + '" fill="none" stroke="rgba(255,255,255,.4)" stroke-width="1"/>';
  s += '<ellipse cx="' + cx + '" cy="' + tcy + '" rx="' + (rx - 16) + '" ry="' + (ry - 5) + '" fill="' + tc.face + '"/>';
  s += '</svg>';
  return s;
}

// "Hiss by Scarlet Beast" felt seal: a glowing occult emblem (inverted pentagram,
// two hissing serpents with forked tongues, curved branding text) drawn as inline SVG
// and laid into the felt as a watermark behind the cards.
var FELT_LOGO_SVG =
'<svg viewBox="0 0 480 480" xmlns="http://www.w3.org/2000/svg">' +
  '<defs>' +
    '<linearGradient id="bone" x1="0" y1="0" x2="0" y2="1">' +
      '<stop offset="0%" stop-color="#f6eed9"/>' +
      '<stop offset="55%" stop-color="#e4d9bb"/>' +
      '<stop offset="100%" stop-color="#bfb189"/>' +
    '</linearGradient>' +
    '<radialGradient id="vig" cx="50%" cy="50%" r="50%">' +
      '<stop offset="0%" stop-color="#000" stop-opacity=".30"/>' +
      '<stop offset="62%" stop-color="#000" stop-opacity=".10"/>' +
      '<stop offset="100%" stop-color="#000" stop-opacity="0"/>' +
    '</radialGradient>' +
    '<filter id="stitch" x="-25%" y="-25%" width="150%" height="150%">' +
      '<feDropShadow dx="0" dy="1.4" stdDeviation="0.5" flood-color="#1a1207" flood-opacity=".8"/>' +
    '</filter>' +
    '<filter id="glow" x="-40%" y="-40%" width="180%" height="180%">' +
      '<feGaussianBlur stdDeviation="2.1" result="b"/>' +
      '<feMerge><feMergeNode in="b"/><feMergeNode in="SourceGraphic"/></feMerge>' +
    '</filter>' +
    '<path id="arcTop" d="M48 240 A192 192 0 0 1 432 240" fill="none"/>' +
    '<path id="arcBot" d="M58 240 A182 182 0 0 0 422 240" fill="none"/>' +
  '</defs>' +
  '<circle cx="240" cy="240" r="224" fill="url(#vig)"/>' +
  '<g fill="none" stroke="url(#bone)" filter="url(#stitch)">' +
    '<circle cx="240" cy="240" r="214" stroke-width="2" opacity=".6"/>' +
    '<circle cx="240" cy="240" r="150" stroke-width="1.2" opacity=".4"/>' +
  '</g>' +
  '<circle cx="240" cy="240" r="206" fill="none" stroke="#c0091f" stroke-width="1" opacity=".5"/>' +
  '<g fill="url(#bone)" filter="url(#stitch)">' +
    '<path d="M40 240 l8 -8 l8 8 l-8 8 z"/>' +
    '<path d="M424 240 l8 -8 l8 8 l-8 8 z"/>' +
  '</g>' +
  '<text class="seal-top" fill="url(#bone)" stroke="#2a1d10" stroke-width=".6" paint-order="stroke" filter="url(#stitch)">' +
    '<textPath href="#arcTop" startOffset="50%" text-anchor="middle">⛧ NON · SERVIAM ⛧</textPath>' +
  '</text>' +
  '<text class="seal-bot" fill="url(#bone)" stroke="#2a1d10" stroke-width=".5" paint-order="stroke" filter="url(#stitch)">' +
    '<textPath href="#arcBot" startOffset="50%" text-anchor="middle">AS ABOVE · SO BELOW</textPath>' +
  '</text>' +
  '<g filter="url(#glow)">' +
    '<circle cx="240" cy="244" r="92" fill="none" stroke="#e21330" stroke-width="2" opacity=".9"/>' +
    '<circle cx="240" cy="244" r="86" fill="none" stroke="url(#bone)" stroke-width="1" opacity=".35"/>' +
    '<polygon points="240,326 191.8,177.7 318,269.3 162,269.3 288.2,177.7" fill="none" stroke="url(#bone)" stroke-width="2.6" stroke-linejoin="round" opacity=".95"/>' +
  '</g>' +
  '<g fill="none" stroke="url(#bone)" stroke-width="6" stroke-linecap="round" opacity=".92" filter="url(#glow)">' +
    '<path d="M228 160 C172 158, 138 200, 142 250 C146 296, 192 320, 232 300"/>' +
    '<path d="M252 160 C308 158, 342 200, 338 250 C334 296, 288 320, 248 300"/>' +
  '</g>' +
  '<g filter="url(#glow)">' +
    '<circle cx="228" cy="160" r="5" fill="#e9ddbf" stroke="#9a0f1f" stroke-width="1.4"/>' +
    '<circle cx="252" cy="160" r="5" fill="#e9ddbf" stroke="#9a0f1f" stroke-width="1.4"/>' +
    '<g stroke="#e21330" stroke-width="2.4" stroke-linecap="round" fill="none">' +
      '<path d="M227 156 l-6 -12 m6 12 l2 -13"/>' +
      '<path d="M253 156 l6 -12 m-6 12 l-2 -13"/>' +
    '</g>' +
  '</g>' +
'</svg>';

// A long sinuous serpent drawn under the cursive brand: green scaled body, little horns,
// glowing red eyes and a forked scarlet tongue.
var SNAKE_SVG =
'<svg viewBox="0 0 740 150" xmlns="http://www.w3.org/2000/svg">' +
  '<defs>' +
    '<linearGradient id="snakeBody" x1="0" y1="0" x2="1" y2="0">' +
      '<stop offset="0%" stop-color="#0a4d2a"/>' +
      '<stop offset="45%" stop-color="#1f9a52"/>' +
      '<stop offset="100%" stop-color="#0e6e3c"/>' +
    '</linearGradient>' +
    '<filter id="snakeShadow" x="-20%" y="-50%" width="140%" height="200%">' +
      '<feDropShadow dx="0" dy="2" stdDeviation="2.2" flood-color="#000" flood-opacity=".55"/>' +
    '</filter>' +
    '<filter id="eyeGlow" x="-200%" y="-200%" width="500%" height="500%">' +
      '<feGaussianBlur stdDeviation="1.6" result="b"/><feMerge><feMergeNode in="b"/><feMergeNode in="SourceGraphic"/></feMerge>' +
    '</filter>' +
  '</defs>' +
  '<path d="M26 80 C120 14,200 138,300 76 C386 24,470 134,560 74 C612 40,656 104,690 70" fill="none" stroke="url(#snakeBody)" stroke-width="17" stroke-linecap="round" filter="url(#snakeShadow)"/>' +
  '<path d="M26 80 C120 14,200 138,300 76 C386 24,470 134,560 74 C612 40,656 104,690 70" fill="none" stroke="#e7dcbb" stroke-width="3" stroke-linecap="round" stroke-dasharray="2 12" opacity=".45"/>' +
  '<path d="M676 58 Q712 54 724 70 Q712 86 676 82 Q690 70 676 58 z" fill="url(#snakeBody)" stroke="#0a4324" stroke-width="1.5" filter="url(#snakeShadow)"/>' +
  '<g stroke="#0a4324" stroke-width="3" stroke-linecap="round" fill="none">' +
    '<path d="M684 58 l-8 -16"/><path d="M696 57 l-1 -18"/>' +
  '</g>' +
  '<g filter="url(#eyeGlow)"><circle cx="694" cy="65" r="3.4" fill="#ff2b2b"/><circle cx="704" cy="68" r="3" fill="#ff2b2b"/></g>' +
  '<g stroke="#e21330" stroke-width="2.6" stroke-linecap="round" fill="none" filter="url(#snakeShadow)">' +
    '<path d="M722 70 l22 -6 M722 70 l22 8 M722 70 l16 1"/>' +
  '</g>' +
'</svg>';

function CardView(props) {
  var value = props.value || '';
  var isBack = value === 'BACK' || value === 'CARD_BACK' || value === '??';
  if (isBack) {
    // Face-down card: dark "satanic" back (inverted pentagram + two intertwined green
    // snakes). Rendered as a PNG so it works regardless of inline-SVG support.
    return e('div', { className: 'card back' },
      e('img', { className: 'back-art', src: '/table-display/assets/cardback.png?v=4', alt: '' })
    );
  }
  var text = value && value !== 'NC' ? value : '';
  if (!text) {
    return e('div', { className: 'card empty' });
  }
  // Render an actual card face: a corner index (rank over suit pip) plus a large
  // centre pip, coloured red for hearts/diamonds and black for clubs/spades.
  var suit = text.slice(-1).toLowerCase();
  var rank = text.slice(0, -1).toUpperCase();
  if (rank === 'T') rank = '10';
  var SUITS = { h: '♥', d: '♦', c: '♣', s: '♠' };
  var pip = SUITS[suit] || '';
  var red = (suit === 'h' || suit === 'd');
  return e('div', { className: 'card face ' + (red ? 'red' : 'black') },
    e('span', { className: 'corner tl' },
      e('span', { className: 'rank' }, rank),
      e('span', { className: 'suit' }, pip)
    ),
    e('span', { className: 'pip' }, pip),
    e('span', { className: 'corner br' },
      e('span', { className: 'rank' }, rank),
      e('span', { className: 'suit' }, pip)
    )
  );
}

function Player(props) {
  var player = props.player;
  var nchairs = props.nchairs;
  var maxHoleCards = props.isOmaha ? 4 : 2;
  var pos = seatPos(nchairs, player.chair);
  // In observer mode p3 is not the hero (we're watching, not playing): render the
  // hero seat as a normal small chair like everyone else.
  var isHero = player.chair === HERO_CHAIR && !props.observer;
  var toggleUnit = props.onToggleUnit;
  var style = { left: (pos[0] * 100) + '%', top: (pos[1] * 100) + '%' };
  var cards = [];
  var rawCards = player.cards || [];
  for (var i = 0; i < rawCards.length && cards.length < maxHoleCards; i += 1) {
    if (rawCards[i] && rawCards[i] !== 'NC') {
      cards.push(rawCards[i]);
    }
  }
  // Dim cards only for a seat that is genuinely OUT *and* still showing its own
  // FACE-UP cards (e.g. the hero after folding, whose cards are still returned).
  // A seat holding face-DOWN cards (cardbacks) is in the hand and must NEVER be
  // dimmed -- the in-hand "active" flag alone is unreliable (a seated player with
  // cardbacks can read active=false), which was greying live players by mistake.
  var isCardBack = function (c) { return c === 'BACK' || c === '??' || c === 'CARD_BACK'; };
  var hasBackCard = cards.some(isCardBack);
  var hasKnownCard = cards.some(function (c) { return !isCardBack(c); });
  var knownFaceCount = cards.filter(function (c) { return !isCardBack(c); }).length;
  var isOut;
  if (isHero) {
    // Hero seat: do NOT grey based on the cardback. It is live whenever it shows
    // no cardback (p3cardback=false) AND both of its face-up hole cards are present
    // (p3cardface0/1 nocard=false). Grey only when those face cards are gone.
    var heroShowingOwnCards = !hasBackCard && knownFaceCount >= 2;
    isOut = player.seated && !heroShowingOwnCards;
  } else {
    isOut = player.seated && !player.active && hasKnownCard && !hasBackCard;
  }
  // The green "active" indicator should mark only the player whose TURN it is
  // (to act), not everyone still in the hand. The backend sends toact (the chair
  // to act); fall back to the in-hand flag only when toact is unknown (-1).
  var isActor = (typeof props.toact === 'number' && props.toact >= 0)
    ? (player.chair === props.toact)
    : player.active;
  return e('div', { className: 'player' + (isHero ? ' hero' : '') + (isOut ? ' out' : ''), style: style },
    e('div', { className: 'seat-ring ' + (isActor ? 'active' : '') }),
    e('div', { className: 'holecards' + (isOut ? ' out' : '') }, cards.map(function (card, index) {
      return e(CardView, { key: index, value: card });
    })),
    e('div', { className: 'name' + (player.matched ? ' matched' : ''), title: player.ptname || '' },
      isActor ? e('span', { className: 'active-dot' }) : null,
      player.name || ('p' + player.chair)
    ),
    e('div', { className: 'balance', onDoubleClick: toggleUnit, title: 'Double-click to toggle BB / $' },
      player.seated ? bb(player.balance) : ('Out (' + bb(player.balance) + ')')),
    e('div', { className: 'hud' },   // [Emrald] no gate: always render the HUD on the React view, like scrcpy
      player.samples >= 0 ? e('span', {
        key: 'samples',
        className: 'hud-samples',
        title: 'PokerTracker 4 hands sampled'
      }, 'n=' + thousands(player.samples)) : null,
      (player.hud || []).map(function (stat, index) {
        return e('span', {
          key: index,
          className: 'hud-stat ' + (stat.important ? 'important' : 'normal'),
          title: stat.name || stat.abbr
        }, (stat.abbr || '') + ' ' + (stat.value || '-'));
      })
    ),
    e(RangeGrid, { player: player })
  );
}

// ---- Opponent range grid: 13x13 hand heat-matrix under the HUD, VPIP%-driven. [Emrald]
// Standard layout: row=first rank, col=second; diagonal=pairs, upper-right=suited, lower-left=offsuit.
// Estimates the player's range as the top VPIP% of hands (by a heuristic preflop strength score,
// combo-weighted). A first pass keyed to VPIP; the line-narrowed (representing) range refines later.
var RG_RANKS = ['A', 'K', 'Q', 'J', 'T', '9', '8', '7', '6', '5', '4', '3', '2'];
var RG_VAL = { 'A': 14, 'K': 13, 'Q': 12, 'J': 11, 'T': 10, '9': 9, '8': 8, '7': 7, '6': 6, '5': 5, '4': 4, '3': 3, '2': 2 };
function rgHand(i, j) {
  var r1 = RG_RANKS[i], r2 = RG_RANKS[j];
  if (i === j) return { label: r1 + r2, combos: 6, kind: 'pair', hi: RG_VAL[r1], lo: RG_VAL[r2] };
  if (i < j) return { label: r1 + r2 + 's', combos: 4, kind: 'suited', hi: RG_VAL[r1], lo: RG_VAL[r2] };
  return { label: r2 + r1 + 'o', combos: 12, kind: 'offsuit', hi: RG_VAL[r2], lo: RG_VAL[r1] };
}
function rgScore(h) {
  if (h.kind === 'pair') return 1000 + h.hi * 12;
  var s = h.hi * 8 + h.lo * 2, gap = h.hi - h.lo - 1;
  if (h.kind === 'suited') s += 28;
  if (gap === 0) s += 18; else if (gap === 1) s += 9; else if (gap === 2) s += 4; else s -= gap * 3;
  if (h.lo >= 10) s += 14;          // both broadway
  if (h.hi === 14) s += 6;          // ace-high kicker
  return s;
}
var RG_SORTED = (function () {
  var all = [];
  for (var i = 0; i < 13; i++) for (var j = 0; j < 13; j++) { var h = rgHand(i, j); h.i = i; h.j = j; h.score = rgScore(h); all.push(h); }
  return all.sort(function (a, b) { return b.score - a.score; });
})();
var rgCache = {};
function rgInRange(pct) {
  var key = Math.round(Math.max(0, Math.min(100, pct)));
  if (rgCache[key]) return rgCache[key];
  var target = 1326 * key / 100, cum = 0, set = {};
  for (var k = 0; k < RG_SORTED.length; k++) { if (cum >= target) break; var h = RG_SORTED[k]; set[h.i * 13 + h.j] = true; cum += h.combos; }
  rgCache[key] = set; return set;
}
function rgPlayerVpip(player) {
  var hud = player.hud || [];
  for (var k = 0; k < hud.length; k++) {
    var a = (hud[k].abbr || '').toLowerCase(), n = (hud[k].name || '').toLowerCase();
    if (a.indexOf('vp') >= 0 || n.indexOf('vpip') >= 0) { var v = parseFloat(hud[k].value); if (!isNaN(v)) return v; }
  }
  return null;
}
// PFR% (preflop-raise) = the player's RAISING / value core, a subset of their VPIP range. Drives the
// inner highlight in the grid (the hands they'd raise, i.e. what an aggressive line represents). [Emrald]
function rgPlayerPfr(player) {
  var hud = player.hud || [];
  for (var k = 0; k < hud.length; k++) {
    var a = (hud[k].abbr || '').toLowerCase(), n = (hud[k].name || '').toLowerCase();
    var isPfr = (a.indexOf('pfr') >= 0 || a === 'pr' || n.indexOf('pfr') >= 0 || (n.indexOf('pre') >= 0 && n.indexOf('rais') >= 0));
    if (isPfr) { var v = parseFloat(hud[k].value); if (!isNaN(v)) return v; }
  }
  return null;
}
var rgOpen = {};   // chair -> expanded? persists across the 500ms table re-renders (remount fallback)
function RangeGrid(props) {
  var chair = props.player.chair;
  var stPair = useState(!!rgOpen[chair]);   // hook FIRST (unconditional) so hook order is stable
  var open = stPair[0], setOpen = stPair[1];
  var vpip = rgPlayerVpip(props.player);
  if (vpip === null || !props.player.seated) return null;   // no PT sample / empty seat -> nothing
  // The RAISING / value core = PFR% (a subset of VPIP). If PFR isn't sampled, estimate it as ~72% of
  // VPIP (a typical micro VPIP/PFR gap). This is the "what their aggressive line represents" inner range.
  var pfr = rgPlayerPfr(props.player);
  if (pfr === null || isNaN(pfr)) pfr = vpip * 0.72;
  if (pfr > vpip) pfr = vpip;
  var toggle = function (ev) { ev.stopPropagation(); rgOpen[chair] = !open; setOpen(!open); };
  var header = e('div', {
    className: 'rg-toggle' + (open ? ' open' : ''),
    onClick: toggle,
    title: 'Estimated range: outer = VPIP ~' + Math.round(vpip) + '% (all hands played); inner gold = PFR ~'
      + Math.round(pfr) + '% (the raising / value core they represent when aggressive) — click to ' + (open ? 'hide' : 'show')
  }, (open ? '▾ ' : '▸ ') + 'range V' + Math.round(vpip) + '% / R' + Math.round(pfr) + '%');
  if (!open) return header;                 // COLLAPSED by default: just the toggle
  var setV = rgInRange(vpip), setP = rgInRange(pfr);
  var cells = [];
  for (var i = 0; i < 13; i++) for (var j = 0; j < 13; j++) {
    var idx = i * 13 + j, h = rgHand(i, j), on = setV[idx], core = setP[idx];
    cells.push(e('div', {
      key: idx,
      className: 'rg-cell' + (on ? ' on ' + h.kind : '') + (core ? ' val' : ''),
      title: h.label + (core ? ' — value/raise' : (on ? ' — call/limp' : ''))
    }));
  }
  return e('div', { className: 'range-wrap' }, header, e('div', { className: 'range-grid' }, cells));
}

// Dealer button, positioned on the felt just in front of the seat (a short hop from the
// chair toward the table centre) so it never sits on top of the player chair.
// An angled (perspective) dealer button chip with a "D" on its face.
function dealerChipSvg() {
  var rx = 24, ry = 8, th = 5, W = 56, padT = 4, padB = 7;
  var H = padT + 2 * ry + th + padB;
  var cx = W / 2, cy = padT + ry;
  return '<svg viewBox="0 0 ' + W + ' ' + H + '" xmlns="http://www.w3.org/2000/svg">' +
    '<defs><filter id="dch" x="-30%" y="-15%" width="160%" height="145%"><feDropShadow dx="0" dy="2" stdDeviation="1.5" flood-color="#000" flood-opacity=".55"/></filter></defs>' +
    '<g filter="url(#dch)">' +
      '<ellipse cx="' + cx + '" cy="' + (cy + th) + '" rx="' + rx + '" ry="' + ry + '" fill="#b89a3e"/>' +
      '<ellipse cx="' + cx + '" cy="' + cy + '" rx="' + rx + '" ry="' + ry + '" fill="#f5ecd2"/>' +
    '</g>' +
    '<ellipse cx="' + cx + '" cy="' + cy + '" rx="' + (rx - 3) + '" ry="' + (ry - 1.4) + '" fill="none" stroke="#caa84e" stroke-width="2.8" stroke-dasharray="3 6.6"/>' +
    '<ellipse cx="' + cx + '" cy="' + cy + '" rx="' + (rx - 9) + '" ry="' + (ry - 3) + '" fill="#fbf4e2"/>' +
    '<g transform="translate(' + cx + ' ' + cy + ') scale(1 0.62) translate(-' + cx + ' -' + cy + ')">' +
      '<text x="' + cx + '" y="' + cy + '" text-anchor="middle" dominant-baseline="central" font-family="Segoe UI, Arial, sans-serif" font-weight="900" font-size="15" fill="#7a1018">D</text>' +
    '</g>' +
  '</svg>';
}

function DealerButton(props) {
  var pos = seatPos(props.nchairs, props.chair);
  var dx = 0.5 - pos[0], dy = 0.49 - pos[1];
  var len = Math.sqrt(dx * dx + dy * dy) || 1;
  var t = 0.17, perp = 0.075;   // a short hop toward centre + offset to the side of the bet
  var x = pos[0] + dx * t + (-dy / len) * perp;
  var y = pos[1] + dy * t + (dx / len) * perp;
  return e('div', {
    className: 'dealer',
    style: { left: (x * 100) + '%', top: (y * 100) + '%' },
    dangerouslySetInnerHTML: { __html: dealerChipSvg() }
  });
}

// A player's bet: a coloured chip + amount, placed out on the felt in front of the seat
// (a third of the way toward the centre) so it sits on the felt for every seat.
function BetChip(props) {
  var pos = seatPos(props.nchairs, props.chair);
  var isHero = props.chair === HERO_CHAIR && !props.observer;
  // The hero seat is large (big hole cards + glowing ring). A normal bet position
  // would land on top of the cards, so push the hero's bet further toward the
  // centre and out to the left (the dealer button already sits to the right),
  // clear of the cards.
  var t = isHero ? 0.50 : 0.34;
  var x = pos[0] + (0.5 - pos[0]) * t;
  var y = pos[1] + (0.49 - pos[1]) * t;
  if (isHero) {
    x -= 0.12;
  }
  return e('div', {
    className: 'bet' + (isHero ? ' hero-bet' : ''),
    style: { left: (x * 100) + '%', top: (y * 100) + '%' },
    onDoubleClick: props.onToggleUnit,
    title: 'Double-click to toggle BB / $'
  },
    e('span', { className: 'bet-chip', dangerouslySetInnerHTML: { __html: chipStackSvg(props.bet) } }),
    e('span', { className: 'bet-amt' }, bb(props.bet))
  );
}

// Red "Matrix" digital rain, painted on a full-window canvas behind the table so it shows
// through the black area around the felt.
function MatrixRain() {
  useEffect(function () {
    var canvas = document.getElementById('hiss-matrix-rain');
    if (!canvas || !canvas.getContext) return;
    var ctx = canvas.getContext('2d');
    var chars = '01アツニ蛇HISSCARLETBEAST☠⛧666$';
    var fontSize = 16;
    var W, H, drops;
    function resize() {
      W = canvas.width = window.innerWidth;
      H = canvas.height = window.innerHeight;
      var cols = Math.max(1, Math.floor(W / fontSize));
      drops = [];
      for (var i = 0; i < cols; i++) drops[i] = Math.floor(Math.random() * -50);
    }
    resize();
    window.addEventListener('resize', resize);
    var raf, last = 0;
    function frame(ts) {
      raf = requestAnimationFrame(frame);
      if (ts - last < 55) return;   // ~18 fps, gentle
      last = ts;
      ctx.fillStyle = 'rgba(3,4,5,0.10)';     // fade the trails
      ctx.fillRect(0, 0, W, H);
      ctx.font = fontSize + 'px monospace';
      for (var i = 0; i < drops.length; i++) {
        var ch = chars.charAt(Math.floor(Math.random() * chars.length));
        var x = i * fontSize, y = drops[i] * fontSize;
        ctx.fillStyle = (Math.random() > 0.972) ? '#ffd2d2' : '#b21422';   // bright heads, dark-red tail
        ctx.fillText(ch, x, y);
        if (y > H && Math.random() > 0.975) drops[i] = 0;
        drops[i] += 1;
      }
    }
    raf = requestAnimationFrame(frame);
    return function () {
      cancelAnimationFrame(raf);
      window.removeEventListener('resize', resize);
    };
  }, []);
  return e('canvas', { id: 'hiss-matrix-rain', className: 'matrix-rain' });
}

// ---- Odds: pot / reverse-pot / implied / reverse-implied, computed CRASH-SAFE from table-state (never
// touches prwin, which can crash Hiss). Implied/reverse-implied use the effective stack as the future-
// money proxy. [Emrald: show the 4 odds on the React table view]
function computeOdds(table) {
  var players = table.players || [];
  var heroChair = (typeof table.userchair === 'number' ? table.userchair : 3);
  var heroBet = 0, heroBal = 0, maxBet = 0, maxOppBal = 0, found = false;
  for (var i = 0; i < players.length; i++) {
    var p = players[i], bet = Number(p.bet || 0), bal = Number(p.balance || 0);
    if (p.chair === heroChair) { heroBet = bet; heroBal = bal; found = true; }
    else { if (bet > maxBet) maxBet = bet; if (bal > maxOppBal) maxOppBal = bal; }
  }
  var pot = Number(table.pot || 0);
  var call = Math.max(0, maxBet - heroBet);
  if (!found || call <= 0 || pot <= 0) return null;
  var eff = Math.min(heroBal || maxOppBal, maxOppBal || heroBal) || 0;
  return {
    call: call, pot: pot,
    po: call / (pot + call),                              // pot odds: equity needed to call now
    rpo: pot / (pot + call),                              // reverse pot odds: the price laid to us
    io: call / (pot + call + eff * 0.35),                 // implied: future bets won when we hit
    rio: (call + eff * 0.25) / (pot + call + eff * 0.25), // reverse implied: future bets paid off behind
    ratio: pot / call
  };
}

function OddsStat(props) {
  return e('div', { title: props.title,
      style: { display: 'flex', flexDirection: 'column', alignItems: 'center', minWidth: '62px',
               padding: '2px 9px', borderRight: '1px solid #ffffff14' } },
    e('span', { style: { fontSize: '9px', letterSpacing: '.5px', color: '#88a0a0', textTransform: 'uppercase' } }, props.label),
    e('span', { style: { fontSize: '14px', fontWeight: 'bold', color: props.color || '#e6e6e6', fontFamily: 'monospace' } }, props.value));
}

function OddsPanel(props) {
  var o = props.odds;
  if (!o) return null;
  var pct = function (x) { return Math.round(x * 100) + '%'; };
  return e('div', { className: 'odds-panel',
      style: { display: 'flex', alignItems: 'stretch', justifyContent: 'center', margin: '6px auto 0',
               background: 'rgba(10,14,12,.72)', border: '1px solid #ffffff1c', borderRadius: '8px',
               padding: '4px 2px', width: 'fit-content', boxShadow: '0 0 10px rgba(0,0,0,.4)' } },
    e(OddsStat, { label: 'Pot odds', value: pct(o.po), color: '#7fd6ff',
        title: 'Equity needed to call now = call/(pot+call). You are getting ' + o.ratio.toFixed(1) + ':1.' }),
    e(OddsStat, { label: 'Rev pot', value: pct(o.rpo), color: '#9ad0a0',
        title: 'Reverse pot odds: the pot share already laid to you (1 - pot odds).' }),
    e(OddsStat, { label: 'Implied', value: pct(o.io), color: '#caa6ff',
        title: 'Implied odds: equity needed counting future bets you WIN when you hit (eff-stack proxy).' }),
    e(OddsStat, { label: 'Rev impl', value: pct(o.rio), color: '#ff9f9f',
        title: 'Reverse implied odds: equity needed counting future chips you PAY OFF when 2nd-best.' }));
}

// ---- RED DECISION overlay, ON FIRE, trailing 10s after each new decision. Shows the brain's CURRENT
// DECIDED ACTION (exploit/branch/mischief), polled from the AIL server's /decision -- a crash-safe DB
// read of brain_state (never re-evaluates the OHF/prwin). [Emrald: red decision on fire, trail 10s]
// ---- PERSISTENT decision chip in the topbar: ALWAYS shows the latest OHF/brain decision (so it's never
// missed even when the bot is quietly folding); glows red when the decision is fresh (<10s). The on-fire
// DecisionOverlay below the board is the dramatic flash; this is the always-on readout. [Emrald]
function DecisionChip() {
  var dPair = useState(null), dec = dPair[0], setDec = dPair[1];
  var nPair = useState(0), setNow = nPair[1];
  useEffect(function () {
    var alive = true;
    function poll() {
      fetch(window.location.protocol + '//' + window.location.hostname + ':7900/decision')
        .then(function (r) { return r.json(); }).then(function (d) { if (alive) setDec(d); }).catch(function () {});
    }
    poll();
    var t = setInterval(poll, 800);
    var tick = setInterval(function () { setNow(Date.now()); }, 500);
    return function () { alive = false; clearInterval(t); clearInterval(tick); };
  }, []);
  if (!dec || !dec.ok || !dec.action) {
    return e('span', { style: { marginLeft: '10px', color: '#666', fontFamily: 'monospace', fontSize: '12px' } }, '🔴 DECISION —');
  }
  var act = (dec.action || '').toUpperCase();
  var label = act + (dec.size_bb ? (' ' + Number(dec.size_bb).toFixed(1) + 'bb') : '');
  var extra = [dec.exploit && dec.exploit !== 'none' ? dec.exploit : null,
               dec.branch && dec.branch !== 'NORMAL' ? ('«' + dec.branch + '»') : null,
               dec.mischief || null].filter(Boolean).join(' · ');
  var age = dec.ts_ms ? (Date.now() - dec.ts_ms) / 1000 : 999;
  var fresh = age < 10;
  var col = act === 'FOLD' ? '#d9a06b'
          : (act.indexOf('RAISE') >= 0 || act.indexOf('ALL') >= 0) ? '#ff5a3c'
          : act === 'CALL' ? '#7fd6ff' : act === 'CHECK' ? '#9ad0a0' : '#e6e6e6';
  return e('span', { title: 'live decision (' + (dec.source || '') + ', ' + Math.round(age) + 's ago)',
      style: { marginLeft: '12px', padding: '3px 12px', borderRadius: '6px', fontFamily: 'monospace',
               fontWeight: 'bold', fontSize: '13px', letterSpacing: '.5px',
               border: '1px solid ' + (fresh ? '#ff5a3c' : '#3a3a3a'),
               background: fresh ? 'rgba(150,15,15,0.22)' : '#161616', color: col,
               boxShadow: fresh ? '0 0 11px rgba(255,70,30,.55)' : 'none' } },
    '🔴 ' + label + (extra ? ('  ·  ' + extra) : ''));
}

function DecisionOverlay() {
  var decPair = useState(null), dec = decPair[0], setDec = decPair[1];
  var shownPair = useState(null), shown = shownPair[0], setShown = shownPair[1];
  var nowPair = useState(0), setNow = nowPair[1];
  useEffect(function () {
    var alive = true;
    function poll() {
      fetch(window.location.protocol + '//' + window.location.hostname + ':7900/decision')
        .then(function (r) { return r.json(); })
        .then(function (d) { if (alive) setDec(d); }).catch(function () {});
    }
    poll();
    var t = setInterval(poll, 600);
    var tick = setInterval(function () { setNow(Date.now()); }, 200);   // drive the flicker + fade
    return function () { alive = false; clearInterval(t); clearInterval(tick); };
  }, []);
  useEffect(function () {
    if (!dec || !dec.ok || !dec.action) return;
    var key = [dec.handnumber, dec.betround, dec.action, dec.size_bb || 0, dec.source || ''].join('|');
    setShown(function (prev) {
      if (prev && prev.key === key) return prev;          // same decision -> keep its 10s timer running
      return { key: key, action: dec.action, size: dec.size_bb, source: dec.source, branch: dec.branch,
               mischief: dec.mischief, exploit: dec.exploit, conf: dec.confidence, energy: dec.energy,
               villain: dec.villain, betround: dec.betround, ts: Date.now() };
    });
  }, [dec]);
  if (!shown) return null;
  var age = (Date.now() - shown.ts) / 1000;
  if (age > 10) return null;                               // trail 10s, then vanish
  var fade = age < 8 ? 1 : (10 - age) / 2;                 // hold 8s, fade the last 2s
  var flick = 0.6 + 0.4 * Math.abs(Math.sin(Date.now() / 120));
  var act = (shown.action || '').toUpperCase();
  var STREETS = ['', 'preflop', 'flop', 'turn', 'river'];
  // The fiery action + the full read behind it: exploit + branch, the villain + confidence, mischief +
  // the energy in the air, and the source/street. [Emrald: more lines, more relevant info]
  var big = '🔥 ' + act + (shown.size ? ('  ' + Number(shown.size).toFixed(1) + 'bb') : '') + ' 🔥';
  var lines = [
    [shown.exploit && shown.exploit !== 'none' ? ('exploit: ' + shown.exploit) : null,
     shown.branch && shown.branch !== 'NORMAL' ? ('« ' + shown.branch + ' »') : null].filter(Boolean).join('   ·   '),
    [shown.villain ? ('vs ' + shown.villain) : null,
     (shown.conf != null) ? ('confidence ' + Math.round(shown.conf * 100) + '%') : null].filter(Boolean).join('   ·   '),
    [shown.mischief ? ('mischief: ' + shown.mischief) : null,
     (shown.energy != null && shown.energy !== 0) ? ('energy ' + Number(shown.energy).toFixed(2)) : null].filter(Boolean).join('   ·   '),
    [(shown.source && shown.source !== 'engine') ? ('via ' + shown.source) : null,
     STREETS[shown.betround] || null].filter(Boolean).join('   ·   ')
  ];
  var cols = ['#ffb199', '#9fd0ff', '#caa6ff', '#e8927a'];
  var szs = ['13px', '12px', '12px', '11px'];
  var kids = [e('div', { key: 'big', style: {
        fontFamily: 'monospace', fontWeight: 'bold', fontSize: '30px', letterSpacing: '3px', color: '#ff5a3c',
        textShadow: '0 0 ' + (6 + 16 * flick) + 'px rgba(255,70,30,' + (0.65 * flick) + '), 0 0 ' + (2 + 6 * flick) + 'px #ff2a00',
        WebkitTextStroke: '1px rgba(120,10,0,.55)' } }, big)];
  lines.forEach(function (ln, i) {
    if (ln) kids.push(e('div', { key: 'l' + i, style: { marginTop: '1px', fontFamily: 'monospace',
        fontSize: szs[i], color: cols[i], letterSpacing: '.8px',
        textShadow: i === 0 ? '0 0 6px rgba(255,80,40,.6)' : 'none' } }, ln));
  });
  return e('div', { className: 'decision-overlay', style: {
        margin: '6px auto 2px', width: '100%', maxWidth: '560px', textAlign: 'center',
        pointerEvents: 'none', opacity: fade, lineHeight: '1.15' } }, kids);
}

function App() {
  var statePair = useState(null);
  var state = statePair[0];
  var setState = statePair[1];
  var errorPair = useState('');
  var error = errorPair[0];
  var setError = errorPair[1];
  var unitPair = useState('bb');
  var unit = unitPair[0];
  var setUnit = unitPair[1];
  var toggleUnit = function () {
    setUnit(function (u) { return u === 'bb' ? 'usd' : 'bb'; });
  };
  var nnPair = useState(false);
  var nnOn = nnPair[0];
  var setNnOn = nnPair[1];
  var toggleNn = function () {
    fetch('/api/nn-driver?on=' + (nnOn ? '0' : '1')).catch(function () {});
    setNnOn(!nnOn);   // optimistic; the poll below reconciles with the real engaged state
  };
  var ultraPair = useState(false);
  var ultraOn = ultraPair[0];
  var setUltraOn = ultraPair[1];
  var toggleUltra = function () {
    fetch('/api/ultra?on=' + (ultraOn ? '0' : '1')).catch(function () {});
    setUltraOn(!ultraOn);   // optimistic; the poll below reconciles
  };

  var superPair = useState(false);
  var superOn = superPair[0];
  var setSuperOn = superPair[1];
  var toggleSuper = function () {
    fetch('/api/superstition?on=' + (superOn ? '0' : '1')).catch(function () {});
    setSuperOn(!superOn);   // optimistic; the poll below reconciles
  };

  // BRAIN: the introspection/intuition harmonizer stack (synapse_map + decision_advisor + aggregators +
  // nervous system). Engaging it makes the seated bot play exploit-adjusted via the knobs -- no rebuild.
  var brainPair = useState(false);
  var brainOn = brainPair[0];
  var setBrainOn = brainPair[1];
  var brainSrv = function (q) {
    var port = window.location.port || '27654';
    return window.location.protocol + '//' + window.location.hostname + ':7900/brain?port=' + port + q;
  };
  var toggleBrain = function () {
    fetch(brainSrv('&on=' + (brainOn ? '0' : '1'))).catch(function () {});
    setBrainOn(!brainOn);   // optimistic; the poll below reconciles via :7900 (the AIL control server)
  };

  // Lobby recon (icm-chip-value skill): navigate to this instance's tournament lobby, read the
  // structure, return to the felt. Runs via the AIL control server (mcp/ail_server.py :7900) so no
  // Hiss rebuild is needed; it kicks off lobby_fetch.sh for THIS page's Hiss port.
  var lobbyPair = useState('');   // '' idle | 'go' fetching
  var lobbyState = lobbyPair[0];
  var setLobbyState = lobbyPair[1];
  var doLobby = function () {
    if (lobbyState === 'go') return;
    setLobbyState('go');
    var port = window.location.port || '27654';
    fetch(window.location.protocol + '//' + window.location.hostname + ':7900/lobby-fetch?port=' + port)
      .catch(function () {})
      .then(function () { setTimeout(function () { setLobbyState(''); }, 26000); });
  };

  // Reflect the NN-driver engaged state (engaging it disengages the autoplayer, server-side).
  useEffect(function () {
    var alive = true;
    function pollNn() {
      fetch('/api/nn-driver').then(function (r) { return r.json(); })
        .then(function (j) { if (alive && j && typeof j.engaged === 'boolean') setNnOn(j.engaged); })
        .catch(function () {});
    }
    pollNn();
    var t = setInterval(pollNn, 1500);
    return function () { alive = false; clearInterval(t); };
  }, []);

  // Reflect the ULTRA-mode engaged state (the audio-driven OHF<->NN meta-controller).
  useEffect(function () {
    var alive = true;
    function pollUltra() {
      fetch('/api/ultra').then(function (r) { return r.json(); })
        .then(function (j) { if (alive && j && typeof j.engaged === 'boolean') setUltraOn(j.engaged); })
        .catch(function () {});
    }
    pollUltra();
    var t = setInterval(pollUltra, 1500);
    return function () { alive = false; clearInterval(t); };
  }, []);

  // Reflect the BRAIN engaged state (the introspection/intuition harmonizer stack).
  useEffect(function () {
    var alive = true;
    function pollBrain() {
      fetch(brainSrv('')).then(function (r) { return r.json(); })
        .then(function (j) { if (alive && j && typeof j.engaged === 'boolean') setBrainOn(j.engaged); })
        .catch(function () {});
    }
    pollBrain();
    var t = setInterval(pollBrain, 1500);
    return function () { alive = false; clearInterval(t); };
  }, []);

  // Reflect the SUPERSTITION (666 Card Oracle) engaged state.
  useEffect(function () {
    var alive = true;
    function pollSuper() {
      fetch('/api/superstition').then(function (r) { return r.json(); })
        .then(function (j) { if (alive && j && typeof j.engaged === 'boolean') setSuperOn(j.engaged); })
        .catch(function () {});
    }
    pollSuper();
    var t = setInterval(pollSuper, 1500);
    return function () { alive = false; clearInterval(t); };
  }, []);

  useEffect(function () {
    var alive = true;
    function load() {
      var xhr = new XMLHttpRequest();
      xhr.open('GET', '/api/table-state?_=' + new Date().getTime(), true);
      xhr.onreadystatechange = function () {
        if (xhr.readyState !== 4 || !alive) {
          return;
        }
        if (xhr.status >= 200 && xhr.status < 300) {
          try {
            setState(JSON.parse(xhr.responseText));
            setError('');
          } catch (e) {
            setError(e.message);
          }
        } else {
          setError('HTTP ' + xhr.status);
        }
      };
      xhr.send();
    }
    load();
    var timer = setInterval(load, 500);
    return function () {
      alive = false;
      clearInterval(timer);
    };
  }, []);

  var table = state || { nchairs: 10, players: [], commonCards: [], limits: {}, pot: 0, handnumber: '' };
  // p3 (chair 3) is the FIXED ground truth for the bottom-centre seat, so anchor
  // the layout to chair 3 always. (The backend's userchair can drift off by one,
  // which pushed p3 one seat to the left; pinning to 3 keeps p3 bottom-centre.)
  HERO_CHAIR = 3;
  var limits = table.limits || {};
  var instancePort = window.location.port || '80';
  // Drive the money formatter: in "usd" mode amounts are big blinds x the big-blind size.
  DISPLAY.unit = unit;
  DISPLAY.bb = Number(limits.bblind || 0);
  return e('main', { className: 'app' },
    e(MatrixRain),
    e('header', { className: 'topbar' },
      e('div', { className: 'title' }, 'Hiss React Table Display — port ' + instancePort),
      e('div', { className: 'meta' },
        e('span', null, 'Hand ' + (table.handnumber || '-')),
        e('span', null, 'Blinds ' + money(limits.sblind) + ' / ' + money(limits.bblind)),
        e('span', { className: 'status' }, error ? ('API: ' + error) : 'API connected'),
        e(DecisionChip)
      ),
      e('button', {
        onClick: toggleNn,
        title: nnOn ? 'NN driver ENGAGED — click to disengage (autoplayer is off)'
                    : 'Engage the NN driver (disengages the autoplayer)',
        style: {
          marginLeft: '12px', padding: '4px 12px', borderRadius: '6px', cursor: 'pointer',
          fontWeight: 'bold', font: 'inherit',
          border: '1px solid ' + (nnOn ? '#3fb950' : '#444'),
          background: nnOn ? '#13351f' : '#1c1c20',
          color: nnOn ? '#3fb950' : '#bbb',
          boxShadow: nnOn ? '0 0 8px rgba(63,185,80,.5)' : 'none'
        }
      }, (nnOn ? '🧠 NN Driver ●' : '🧠 NN Driver')),
      e('button', {
        onClick: toggleBrain,
        title: brainOn ? 'BRAIN ENGAGED — introspection + intuition steering the bot via the knobs; click to disengage'
                       : 'Engage the BRAIN: per-opponent introspection + intuition + the exploit advisor (no rebuild)',
        style: {
          marginLeft: '8px', padding: '4px 12px', borderRadius: '6px', cursor: 'pointer',
          fontWeight: 'bold', font: 'inherit',
          border: '1px solid ' + (brainOn ? '#2bd4d4' : '#444'),
          background: brainOn ? '#0e3434' : '#1c1c20',
          color: brainOn ? '#2bd4d4' : '#bbb',
          boxShadow: brainOn ? '0 0 8px rgba(43,212,212,.6)' : 'none'
        }
      }, (brainOn ? '🧠💭 Brain ●' : '🧠💭 Brain')),
      e('button', {
        onClick: toggleUltra,
        title: ultraOn ? 'ULTRA ENGAGED — sound-driven OHF<->NN switching; click to stop'
                       : 'ULTRA mode: randomly flip OHF<->NN from the system-audio average',
        style: {
          marginLeft: '8px', padding: '4px 12px', borderRadius: '6px', cursor: 'pointer',
          fontWeight: 'bold', font: 'inherit',
          border: '1px solid ' + (ultraOn ? '#d24bd2' : '#444'),
          background: ultraOn ? '#2a1335' : '#1c1c20',
          color: ultraOn ? '#e26be2' : '#bbb',
          boxShadow: ultraOn ? '0 0 8px rgba(210,75,210,.6)' : 'none'
        }
      }, (ultraOn ? '⚡ ULTRA ●' : '⚡ ULTRA')),
      (function () {
        var fav = Number(table.beastfavor || 0);
        var live = fav > 0.001;          // omen actively reading a live draw
        var on = superOn || live;        // lit when superstition is ENGAGED (toggle) or the omen is live
        var pct = Math.round(fav * 100);
        var verdict = fav >= 0.66 ? 'THE BEAST FAVORS YOU' : (fav >= 0.33 ? 'the signal is faint' : 'the omen turns away');
        return e('div', {
          onClick: toggleSuper,
          title: superOn
            ? ('Superstition ENGAGED - 666 Card Oracle feeding the OHF'
               + (live ? (' (resonance ' + pct + '%: ' + verdict + ')') : ' (no live draw)') + ' - click to STOP')
            : 'Superstition OFF - click to ENGAGE the 666 Card Oracle (feeds the OHF + voices omens)',
          style: {
            marginLeft: '10px', padding: '4px 10px', borderRadius: '6px', fontWeight: 'bold', cursor: 'pointer',
            fontFamily: 'monospace', letterSpacing: '1px',
            border: '1px solid ' + (on ? '#e23b3b' : '#333'),
            background: on ? ('rgba(150,15,15,' + (0.18 + 0.5 * fav) + ')') : '#161616',
            color: on ? '#ff6b6b' : '#555',
            boxShadow: on ? ('0 0 ' + Math.round(4 + 14 * fav) + 'px rgba(226,59,59,' + (0.3 + 0.6 * fav) + ')') : 'none'
          }
        }, superOn ? ('🔥 666 ●' + (live ? (' ' + pct + '%') : '')) : '🔥 666');
      })(),
      e('button', {
        onClick: doLobby,
        title: lobbyState === 'go'
          ? 'Lobby recon in progress — navigating to the lobby, reading the structure, returning to the felt…'
          : 'Lobby recon: scout this table\'s tournament lobby (blinds / players left / prize pool) into the ICM model, then return to the felt',
        style: {
          marginLeft: '10px', padding: '4px 10px', borderRadius: '6px', cursor: 'pointer',
          fontWeight: 'bold', font: 'inherit',
          border: '1px solid ' + (lobbyState === 'go' ? '#e8b84b' : '#444'),
          background: lobbyState === 'go' ? '#352a13' : '#1c1c20',
          color: lobbyState === 'go' ? '#f0c75e' : '#bbb',
          boxShadow: lobbyState === 'go' ? '0 0 8px rgba(232,184,75,.55)' : 'none'
        }
      }, (lobbyState === 'go' ? '🔭 Lobby…' : '🔭 Lobby'))
    ),
    e('section', { className: 'table' },
      e('div', { className: 'felt' },
        e('div', { className: 'felt-logo', dangerouslySetInnerHTML: { __html: FELT_LOGO_SVG } }),
        e('div', { className: 'felt-script' },
          e('span', { className: 'hiss' }, 'Hiss'), ' by Scarlet Beast'),
        e('div', { className: 'felt-snake', dangerouslySetInnerHTML: { __html: SNAKE_SVG } })
      ),
      e('div', { className: 'center' },
        e('div', { className: 'cards' }, (table.commonCards || []).map(function (card, index) {
          return e(CardView, { key: index, value: card });
        })),
        e(DecisionOverlay),
        e('div', { className: 'pot', onDoubleClick: toggleUnit, title: 'Double-click to toggle BB / $' }, 'Pot ' + bb(table.pot)),
        e(OddsPanel, { odds: computeOdds(table) })
      ),
      (table.players || []).map(function (player) {
        return e(Player, { key: player.chair, player: player, nchairs: table.nchairs, toact: (typeof table.toact === 'number' ? table.toact : -1), isOmaha: !!table.isomaha, observer: !!table.observer, onToggleUnit: toggleUnit });
      }),
      (table.players || []).filter(function (p) { return p.dealer; }).map(function (p) {
        return e(DealerButton, { key: 'd' + p.chair, chair: p.chair, nchairs: table.nchairs });
      }),
      (table.players || []).filter(function (p) { return p.bet; }).map(function (p) {
        return e(BetChip, { key: 'b' + p.chair, chair: p.chair, nchairs: table.nchairs, bet: p.bet, observer: !!table.observer, onToggleUnit: toggleUnit });
      })
    )
  );
}

ReactDOM.createRoot(document.getElementById('root')).render(e(App));
