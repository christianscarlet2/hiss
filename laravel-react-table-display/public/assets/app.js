var useEffect = React.useEffect;
var useState = React.useState;

var pc = [
  [], [], [[.95,.47],[.05,.47]],
  [[.95,.47],[.50,.83],[.05,.47]],
  [[.89,.25],[.89,.69],[.11,.69],[.11,.25]],
  [[.89,.25],[.89,.69],[.50,.83],[.11,.69],[.11,.25]],
  [[.72,.11],[.95,.47],[.72,.83],[.28,.83],[.05,.47],[.28,.11]],
  [[.72,.11],[.95,.47],[.72,.83],[.50,.83],[.28,.83],[.05,.47],[.28,.11]],
  [[.72,.11],[.89,.25],[.89,.69],[.72,.83],[.28,.83],[.11,.69],[.11,.25],[.28,.11]],
  [[.72,.11],[.89,.25],[.89,.69],[.72,.83],[.50,.83],[.28,.83],[.11,.69],[.11,.25],[.28,.11]],
  [[.72,.11],[.85,.21],[.95,.47],[.85,.73],[.72,.83],[.28,.83],[.15,.73],[.05,.47],[.15,.21],[.28,.11]]
];

function e(type, props) {
  var args = [type, props || {}];
  for (var i = 2; i < arguments.length; i += 1) {
    args.push(arguments[i]);
  }
  return React.createElement.apply(React, args);
}

function money(value) {
  value = Number(value || 0);
  return value.toFixed(2);
}

function thousands(value) {
  value = Math.max(0, Math.round(Number(value || 0)));
  return String(value).replace(/\B(?=(\d{3})+(?!\d))/g, ',');
}

function CardView(props) {
  var value = props.value || '';
  var text = value && value !== 'NC' ? value : '';
  var isBack = value === 'BACK' || value === 'CARD_BACK' || value === '??';
  var red = /[hd]$/i.test(text);
  return e('div', { className: 'card ' + (!text ? 'empty ' : '') + (isBack ? 'back ' : '') + (red ? 'red' : '') }, isBack ? '*' : text);
}

function Player(props) {
  var player = props.player;
  var nchairs = props.nchairs;
  var maxHoleCards = props.isOmaha ? 4 : 2;
  var pos = pc[nchairs] && pc[nchairs][player.chair] ? pc[nchairs][player.chair] : [.5, .5];
  var style = { left: (pos[0] * 100) + '%', top: (pos[1] * 100) + '%' };
  var cards = [];
  var rawCards = player.cards || [];
  for (var i = 0; i < rawCards.length && cards.length < maxHoleCards; i += 1) {
    if (rawCards[i] && rawCards[i] !== 'NC') {
      cards.push(rawCards[i]);
    }
  }
  return e('div', { className: 'player', style: style },
    player.bet ? e('div', { className: 'bet' }, money(player.bet)) : null,
    e('div', { className: 'seat-ring ' + (player.active ? 'active' : '') }),
    e('div', { className: 'holecards' }, cards.map(function (card, index) {
      return e(CardView, { key: index, value: card });
    })),
    e('div', { className: 'name' + (player.matched ? ' matched' : ''), title: player.ptname || '' },
      player.active ? e('span', { className: 'active-dot' }) : null,
      player.name || ('p' + player.chair)
    ),
    e('div', { className: 'balance' }, player.seated ? money(player.balance) : ('Out (' + money(player.balance) + ')')),
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
    ) : null,
    player.dealer ? e('div', { className: 'dealer' }, 'D') : null
  );
}

function App() {
  var statePair = useState(null);
  var state = statePair[0];
  var setState = statePair[1];
  var errorPair = useState('');
  var error = errorPair[0];
  var setError = errorPair[1];

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
  return e('main', { className: 'app' },
    e('header', { className: 'topbar' },
      e('div', { className: 'title' }, 'OpenHoldem React Table Display — port ' + instancePort),
      e('div', { className: 'meta' },
        e('span', null, 'Hand ' + (table.handnumber || '-')),
        e('span', null, 'Blinds ' + money(limits.sblind) + ' / ' + money(limits.bblind)),
        e('span', { className: 'status' }, error ? ('API: ' + error) : 'API connected')
      )
    ),
    e('section', { className: 'table' },
      e('div', { className: 'felt' }),
      e('div', { className: 'center' },
        e('div', { className: 'cards' }, (table.commonCards || []).map(function (card, index) {
          return e(CardView, { key: index, value: card });
        })),
        e('div', { className: 'pot' }, 'Pot ' + money(table.pot))
      ),
      (table.players || []).map(function (player) {
        return e(Player, { key: player.chair, player: player, nchairs: table.nchairs, isOmaha: !!table.isomaha });
      })
    )
  );
}

ReactDOM.createRoot(document.getElementById('root')).render(e(App));
