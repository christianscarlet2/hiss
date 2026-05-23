(function (global) {
  function applyProps(element, props) {
    Object.keys(props || {}).forEach(function (key) {
      var value = props[key];
      if (key === 'className') {
        element.setAttribute('class', value);
      } else if (key === 'style' && value && typeof value === 'object') {
        Object.keys(value).forEach(function (styleKey) {
          element.style[styleKey] = value[styleKey];
        });
      } else if (key.indexOf('on') === 0 && typeof value === 'function') {
        element.addEventListener(key.substring(2).toLowerCase(), value);
      } else if (value !== false && value !== null && value !== undefined) {
        element.setAttribute(key, value);
      }
    });
  }

  function renderNode(node) {
    if (typeof node === 'string' || typeof node === 'number') {
      return document.createTextNode(node);
    }
    if (!node) {
      return document.createTextNode('');
    }
    if (typeof node.type === 'function') {
      return renderNode(node.type(node.props || {}));
    }
    var element = document.createElement(node.type);
    applyProps(element, node.props);
    (node.children || []).forEach(function (child) {
      element.appendChild(renderNode(child));
    });
    return element;
  }

  global.ReactDOM = {
    createRoot: function (container) {
      var current = null;
      function draw(node) {
        current = node;
        global.React.__resetHooks();
        container.innerHTML = '';
        container.appendChild(renderNode(current));
      }
      global.React.__setRerender(function () {
        if (current) {
          draw(current);
        }
      });
      return { render: draw };
    }
  };
}(window));
