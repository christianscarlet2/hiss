var useEffect = React.useEffect;
var useState = React.useState;
var useRef = React.useRef;

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
  var isHero = player.chair === HERO_CHAIR;
  var style = { left: (pos[0] * 100) + '%', top: (pos[1] * 100) + '%' };
  var cards = [];
  var rawCards = player.cards || [];
  for (var i = 0; i < rawCards.length && cards.length < maxHoleCards; i += 1) {
    if (rawCards[i] && rawCards[i] !== 'NC') {
      cards.push(rawCards[i]);
    }
  }
  var toggleUnit = props.onToggleUnit;
  return e('div', { className: 'player' + (isHero ? ' hero' : ''), style: style },
    player.bet ? e('div', { className: 'bet', onDoubleClick: toggleUnit, title: 'Double-click to toggle BB / $' }, bb(player.bet)) : null,
    e('div', { className: 'seat-ring ' + (player.active ? 'active' : '') }),
    e('div', { className: 'holecards' }, cards.map(function (card, index) {
      return e(CardView, { key: index, value: card });
    })),
    e('div', { className: 'name' + (player.matched ? ' matched' : ''), title: player.ptname || '' },
      player.active ? e('span', { className: 'active-dot' }) : null,
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
function DealerButton(props) {
  var pos = seatPos(props.nchairs, props.chair);
  var t = 0.26;   // fraction of the way from the seat toward the centre
  var x = pos[0] + (0.5 - pos[0]) * t;
  var y = pos[1] + (0.49 - pos[1]) * t;
  return e('div', { className: 'dealer', style: { left: (x * 100) + '%', top: (y * 100) + '%' } }, 'D');
}

// Red "Matrix" digital rain, painted on a full-window canvas behind the table so it shows
// through the black area around the felt.
function MatrixRain() {
  var ref = useRef(null);
  useEffect(function () {
    var canvas = ref.current;
    if (!canvas) return;
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
  return e('canvas', { className: 'matrix-rain', ref: ref });
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
      )
    ),
    e('section', { className: 'table' },
      e('div', { className: 'felt' },
        e('div', { className: 'felt-logo', dangerouslySetInnerHTML: { __html: FELT_LOGO_SVG } }),
        e('div', { className: 'felt-script' }, 'Hiss by Scarlet Beast'),
        e('div', { className: 'felt-snake', dangerouslySetInnerHTML: { __html: SNAKE_SVG } })
      ),
      e('div', { className: 'center' },
        e('div', { className: 'cards' }, (table.commonCards || []).map(function (card, index) {
          return e(CardView, { key: index, value: card });
        })),
        e('div', { className: 'pot', onDoubleClick: toggleUnit, title: 'Double-click to toggle BB / $' }, 'Pot ' + bb(table.pot))
      ),
      (table.players || []).map(function (player) {
        return e(Player, { key: player.chair, player: player, nchairs: table.nchairs, isOmaha: !!table.isomaha, onToggleUnit: toggleUnit });
      }),
      (table.players || []).filter(function (p) { return p.dealer; }).map(function (p) {
        return e(DealerButton, { key: 'd' + p.chair, chair: p.chair, nchairs: table.nchairs });
      })
    )
  );
}

ReactDOM.createRoot(document.getElementById('root')).render(e(App));
