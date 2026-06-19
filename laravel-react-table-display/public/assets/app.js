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
    (player.samples >= 0 || (player.hud || []).length) ? e('div', { className: 'hud' },
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
    ) : null
  );
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
        e('span', { className: 'status' }, error ? ('API: ' + error) : 'API connected')
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
      }, (ultraOn ? '⚡ ULTRA ●' : '⚡ ULTRA'))
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
        e('div', { className: 'pot', onDoubleClick: toggleUnit, title: 'Double-click to toggle BB / $' }, 'Pot ' + bb(table.pot))
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
