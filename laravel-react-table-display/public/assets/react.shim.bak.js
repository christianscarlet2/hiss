(function (global) {
  function flatten(input, output) {
    for (var i = 0; i < input.length; i += 1) {
      var item = input[i];
      if (Array.isArray(item)) {
        flatten(item, output);
      } else if (item !== null && item !== undefined && item !== false) {
        output.push(item);
      }
    }
  }

  function createElement(type, props) {
    var children = [];
    flatten(Array.prototype.slice.call(arguments, 2), children);
    return { type: type, props: props || {}, children: children };
  }

  var hookState = [];
  var hookIndex = 0;
  var rerender = null;

  function useState(initialValue) {
    var index = hookIndex;
    hookIndex += 1;
    if (hookState.length <= index) {
      hookState[index] = initialValue;
    }
    function setState(nextValue) {
      hookState[index] = typeof nextValue === 'function' ? nextValue(hookState[index]) : nextValue;
      if (rerender) {
        rerender();
      }
    }
    return [hookState[index], setState];
  }

  function useEffect(callback) {
    if (hookState.__effectStarted) {
      return;
    }
    hookState.__effectStarted = true;
    setTimeout(callback, 0);
  }

  global.React = {
    createElement: createElement,
    useState: useState,
    useEffect: useEffect,
    __resetHooks: function () { hookIndex = 0; },
    __setRerender: function (fn) { rerender = fn; }
  };
}(window));
