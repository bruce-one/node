// Copyright Joyent, Inc. and other Node contributors.
//
// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the
// "Software"), to deal in the Software without restriction, including
// without limitation the rights to use, copy, modify, merge, publish,
// distribute, sublicense, and/or sell copies of the Software, and to permit
// persons to whom the Software is furnished to do so, subject to the
// following conditions:
//
// The above copyright notice and this permission notice shall be included
// in all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
// OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN
// NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
// DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
// OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
// USE OR OTHER DEALINGS IN THE SOFTWARE.

'use strict';

const { runInDebugContext, runInNewContext } = require('vm');
const errors = require('internal/errors');
const { TextDecoder, TextEncoder } = require('internal/encoding');
const { isBuffer } = require('buffer').Buffer;

const { errname } = process.binding('uv');

const {
  getPromiseDetails,
  getProxyDetails,
  getValueKind,
  isRegExp,
  isDate,
  kPending,
  kRejected,
} = process.binding('util');

const { isArray } = Array;

const {
  uncurryThis
} = require('internal/util/types');

const {
  isDeepStrictEqual
} = require('internal/util/comparisons');

// Which of these can be fooled? how?
const {
  customInspectSymbol,
  deprecate,
  getConstructorOf,
  getFunctionName,
  getProperty,
  getPropertyDescriptor,
  isError,
  promisify,
  join
} = require('internal/util');

const inspectDefaultOptions = Object.seal({
  showHidden: false,
  depth: 2,
  colors: false,
  customInspect: true,
  showProxy: false,
  maxArrayLength: 100,
  breakLength: 60,
  allowSideEffects: true,
});

var CIRCULAR_ERROR_MESSAGE;
var Debug;

const ArrayBufferByteLength = uncurryThis(
  getPropertyDescriptor(ArrayBuffer.prototype, 'byteLength').get);
const BooleanValueOf = uncurryThis(Boolean.prototype.valueOf);
const DataViewByteLength = uncurryThis(
  getPropertyDescriptor(DataView.prototype, 'byteLength').get);
const DataViewByteOffset = uncurryThis(
  getPropertyDescriptor(DataView.prototype, 'byteOffset').get);
const DataViewBuffer = uncurryThis(
  getPropertyDescriptor(DataView.prototype, 'buffer').get);
const DateGetTime = uncurryThis(Date.prototype.getTime);
const DateToString = uncurryThis(Date.prototype.toString);
const DateToISOString = uncurryThis(Date.prototype.toISOString);
const ErrorToString = uncurryThis(Error.prototype.toString);
const HasOwnProperty = uncurryThis(Object.prototype.hasOwnProperty);
const MapSize = uncurryThis(
  getPropertyDescriptor(Map.prototype, 'size').get);
const NumberValueOf = uncurryThis(Number.prototype.valueOf);
const PropertyIsEnumerable = uncurryThis(Object.prototype.propertyIsEnumerable);
const RegExpTest = uncurryThis(RegExp.prototype.test);
const RegExpToString = uncurryThis(RegExp.prototype.toString);
const SetSize = uncurryThis(
  getPropertyDescriptor(Set.prototype, 'size').get);
const SharedArrayBufferByteLength = uncurryThis(
  getPropertyDescriptor(global.SharedArrayBuffer.prototype, 'byteLength').get);
const StringReplace = uncurryThis(String.prototype.replace);
const StringCharCodeAt = uncurryThis(String.prototype.charCodeAt);
const StringSlice = uncurryThis(String.prototype.slice);
const StringValueOf = uncurryThis(String.prototype.valueOf);
const SymbolValueOf = uncurryThis(Symbol.prototype.valueOf);
const SymbolToString = uncurryThis(Symbol.prototype.toString);
const TypedArrayLength = uncurryThis(
  getPropertyDescriptor(Uint8Array.prototype, 'length').get);

const { min, max } = Math;

const { getOwnPropertyNames, getOwnPropertySymbols, keys: Keys } = Object;

let SafeGlobal;
function getSafeGlobal() {
  if (SafeGlobal !== undefined)
    return SafeGlobal;
  return SafeGlobal = runInNewContext('({ Array, Map, Object, Set })');
}

/* eslint-disable */
const strEscapeSequencesRegExp = /[\x00-\x1f\x27\x5c]/;
const keyEscapeSequencesRegExp = /[\x00-\x1f\x27]/;
const strEscapeSequencesReplacer = /[\x00-\x1f\x27\x5c]/g;
const keyEscapeSequencesReplacer = /[\x00-\x1f\x27]/g;
/* eslint-enable */
const keyStrRegExp = /^[a-zA-Z_][a-zA-Z_0-9]*$/;
const colorRegExp = /\u001b\[\d\d?m/g;
const numberRegExp = /^(0|[1-9][0-9]*)$/;

// Escaped special characters. Use empty strings to fill up unused entries.
const meta = [
  '\\u0000', '\\u0001', '\\u0002', '\\u0003', '\\u0004',
  '\\u0005', '\\u0006', '\\u0007', '\\b', '\\t',
  '\\n', '\\u000b', '\\f', '\\r', '\\u000e',
  '\\u000f', '\\u0010', '\\u0011', '\\u0012', '\\u0013',
  '\\u0014', '\\u0015', '\\u0016', '\\u0017', '\\u0018',
  '\\u0019', '\\u001a', '\\u001b', '\\u001c', '\\u001d',
  '\\u001e', '\\u001f', '', '', '',
  '', '', '', '', "\\'", '', '', '', '', '',
  '', '', '', '', '', '', '', '', '', '',
  '', '', '', '', '', '', '', '', '', '',
  '', '', '', '', '', '', '', '', '', '',
  '', '', '', '', '', '', '', '', '', '',
  '', '', '', '', '', '', '', '\\\\'
];

const escapeFn = (str) => meta[str.charCodeAt(0)];

// Escape control characters, single quotes and the backslash.
// This is similar to JSON stringify escaping.
function strEscape(str) {
  // Some magic numbers that worked out fine while benchmarking with v8 6.0
  if (str.length < 5000 && !RegExpTest(strEscapeSequencesRegExp, str))
    return `'${str}'`;
  if (str.length > 100)
    return `'${StringReplace(str, strEscapeSequencesReplacer, escapeFn)}'`;
  var result = '';
  var last = 0;
  for (var i = 0; i < str.length; i++) {
    const point = StringCharCodeAt(str, i);
    if (point === 39 || point === 92 || point < 32) {
      if (last === i) {
        result += meta[point];
      } else {
        result += `${StringSlice(str, last, i)}${meta[point]}`;
      }
      last = i + 1;
    }
  }
  if (last === 0) {
    result = str;
  } else if (last !== i) {
    result += str.slice(last);
  }
  return `'${result}'`;
}

// Escape control characters and single quotes.
// Note: for performance reasons this is not combined with strEscape
function keyEscape(str) {
  if (str.length < 5000 && !RegExpTest(keyEscapeSequencesRegExp, str))
    return `'${str}'`;
  if (str.length > 100)
    return `'${StringReplace(str, keyEscapeSequencesReplacer, escapeFn)}'`;
  var result = '';
  var last = 0;
  for (var i = 0; i < str.length; i++) {
    const point = StringCharCodeAt(str, i);
    if (point === 39 || point < 32) {
      if (last === i) {
        result += meta[point];
      } else {
        result += `${StringSlice(str, last, i)}${meta[point]}`;
      }
      last = i + 1;
    }
  }
  if (last === 0) {
    result = str;
  } else if (last !== i) {
    result += StringSlice(str, last);
  }
  return `'${result}'`;
}

function tryStringify(arg) {
  try {
    return JSON.stringify(arg);
  } catch (err) {
    // Populate the circular error message lazily
    if (!CIRCULAR_ERROR_MESSAGE) {
      try {
        const a = {}; a.a = a; JSON.stringify(a);
      } catch (err) {
        CIRCULAR_ERROR_MESSAGE = err.message;
      }
    }
    if (err.name === 'TypeError' && err.message === CIRCULAR_ERROR_MESSAGE)
      return '[Circular]';
    throw err;
  }
}

function format(f) {
  var i, tempStr;
  if (typeof f !== 'string') {
    if (arguments.length === 0) return '';
    var res = '';
    for (i = 0; i < arguments.length - 1; i++) {
      res += inspect(arguments[i]);
      res += ' ';
    }
    res += inspect(arguments[i]);
    return res;
  }

  if (arguments.length === 1) return f;

  var str = '';
  var a = 1;
  var lastPos = 0;
  for (i = 0; i < f.length - 1; i++) {
    if (f.charCodeAt(i) === 37) { // '%'
      const nextChar = f.charCodeAt(++i);
      if (a !== arguments.length) {
        switch (nextChar) {
          case 115: // 's'
            tempStr = String(arguments[a++]);
            break;
          case 106: // 'j'
            tempStr = tryStringify(arguments[a++]);
            break;
          case 100: // 'd'
            tempStr = `${Number(arguments[a++])}`;
            break;
          case 79: // 'O'
            tempStr = inspect(arguments[a++]);
            break;
          case 111: // 'o'
            tempStr = inspect(arguments[a++],
                              { showHidden: true, depth: 4, showProxy: true });
            break;
          case 105: // 'i'
            tempStr = `${parseInt(arguments[a++])}`;
            break;
          case 102: // 'f'
            tempStr = `${parseFloat(arguments[a++])}`;
            break;
          case 37: // '%'
            str += f.slice(lastPos, i);
            lastPos = i + 1;
            continue;
          default: // any other character is not a correct placeholder
            continue;
        }
        if (lastPos !== i - 1)
          str += f.slice(lastPos, i - 1);
        str += tempStr;
        lastPos = i + 1;
      } else if (nextChar === 37) {
        str += f.slice(lastPos, i);
        lastPos = i + 1;
      }
    }
  }
  if (lastPos === 0)
    str = f;
  else if (lastPos < f.length)
    str += f.slice(lastPos);
  while (a < arguments.length) {
    const x = arguments[a++];
    if ((typeof x !== 'object' && typeof x !== 'symbol') || x === null) {
      str += ` ${x}`;
    } else {
      str += ` ${inspect(x)}`;
    }
  }
  return str;
}

var debugs = {};
var debugEnviron;

function debuglog(set) {
  if (debugEnviron === undefined) {
    debugEnviron = new Set(
      (process.env.NODE_DEBUG || '').split(',').map((s) => s.toUpperCase()));
  }
  set = set.toUpperCase();
  if (!debugs[set]) {
    if (debugEnviron.has(set)) {
      var pid = process.pid;
      debugs[set] = function() {
        var msg = exports.format.apply(exports, arguments);
        console.error('%s %d: %s', set, pid, msg);
      };
    } else {
      debugs[set] = function() {};
    }
  }
  return debugs[set];
}

/**
 * Echos the value of a value. Tries to print the value out
 * in the best way possible given the different types.
 *
 * @param {Object} obj The object to print out.
 * @param {Object} opts Optional options object that alters the output.
 */
/* Legacy: obj, showHidden, depth, colors*/
function inspect(obj, opts) {
  // Default options
  const ctx = {
    seen: [],
    stylize: stylizeNoColor,
    showHidden: inspectDefaultOptions.showHidden,
    depth: inspectDefaultOptions.depth,
    colors: inspectDefaultOptions.colors,
    customInspect: inspectDefaultOptions.customInspect,
    showProxy: inspectDefaultOptions.showProxy,
    maxArrayLength: inspectDefaultOptions.maxArrayLength,
    breakLength: inspectDefaultOptions.breakLength,
    allowSideEffects: inspectDefaultOptions.allowSideEffects,
    indentationLvl: 0,
    createSafeArray(n) {
      if (this.allowSideEffects) {
        if (n === undefined)
          return [];
        return new Array(n);
      }
      {
        const { Array } = getSafeGlobal();
        if (n === undefined)
          return new Array();
        return new Array(n);
      }
    },
    getKeys(obj) {
      if (this.allowSideEffects)
        return Keys(obj);
      return getSafeGlobal().Object.keys(obj);
    },
    getOwnPropertyNames(obj) {
      if (this.allowSideEffects)
        return getOwnPropertyNames(obj);
      return getSafeGlobal().Object.getOwnPropertyNames(obj);
    },
    getOwnPropertySymbols(obj) {
      if (this.allowSideEffects)
        return getOwnPropertySymbols(obj);
      return getSafeGlobal().Object.getOwnPropertySymbols(obj);
    },
    getMapIterable(value) {
      if (this.allowSideEffects)
        return value;
      return getSafeGlobal().Map.prototype[Symbol.iterator].call(value);
    },
    getSetIterable(value) {
      if (this.allowSideEffects)
        return value;
      return getSafeGlobal().Set.prototype[Symbol.iterator].call(value);
    }
  };
  // Legacy...
  if (arguments.length > 2) {
    if (arguments[2] !== undefined) {
      ctx.depth = arguments[2];
    }
    if (arguments.length > 3 && arguments[3] !== undefined) {
      ctx.colors = arguments[3];
    }
  }
  // Set user-specified options
  if (typeof opts === 'boolean') {
    ctx.showHidden = opts;
  } else if (opts) {
    const optKeys = Object.keys(opts);
    for (var i = 0; i < optKeys.length; i++) {
      ctx[optKeys[i]] = opts[optKeys[i]];
    }
  }
  if (ctx.colors) ctx.stylize = stylizeWithColor;
  if (ctx.maxArrayLength === null) ctx.maxArrayLength = Infinity;
  if (!ctx.allowSideEffects) {
    ctx.showProxy = true;
    ctx.customInspect = false;
    ctx.seen = ctx.createSafeArray();
  }
  return formatValue(ctx, obj, ctx.depth);
}
inspect.custom = customInspectSymbol;

Object.defineProperty(inspect, 'defaultOptions', {
  get() {
    return inspectDefaultOptions;
  },
  set(options) {
    if (options === null || typeof options !== 'object') {
      throw new errors.TypeError('ERR_INVALID_ARG_TYPE', 'options', 'object');
    }
    Object.assign(inspectDefaultOptions, options);
    return inspectDefaultOptions;
  }
});

// http://en.wikipedia.org/wiki/ANSI_escape_code#graphics
inspect.colors = Object.assign(Object.create(null), {
  'bold': [1, 22],
  'italic': [3, 23],
  'underline': [4, 24],
  'inverse': [7, 27],
  'white': [37, 39],
  'grey': [90, 39],
  'black': [30, 39],
  'blue': [34, 39],
  'cyan': [36, 39],
  'green': [32, 39],
  'magenta': [35, 39],
  'red': [31, 39],
  'yellow': [33, 39]
});

// Don't use 'blue' not visible on cmd.exe
inspect.styles = Object.assign(Object.create(null), {
  'special': 'cyan',
  'number': 'yellow',
  'boolean': 'yellow',
  'undefined': 'grey',
  'null': 'bold',
  'string': 'green',
  'symbol': 'green',
  'date': 'magenta',
  // "name": intentionally not styling
  'regexp': 'red'
});

function stylizeWithColor(str, styleType) {
  const style = inspect.styles[styleType];
  if (style !== undefined) {
    const color = inspect.colors[style];
    return `\u001b[${color[0]}m${str}\u001b[${color[1]}m`;
  }
  return str;
}

function stylizeNoColor(str, styleType) {
  return str;
}

function ensureDebugIsInitialized() {
  if (Debug === undefined) {
    // a workaround till this entire method is removed
    const originalValue = process.noDeprecation;
    process.noDeprecation = true;
    Debug = runInDebugContext('Debug');
    process.noDeprecation = originalValue;
  }
}

function formatValue(ctx, value, recurseTimes, ln) {
  // Primitive types cannot have properties
  if (typeof value !== 'object' && typeof value !== 'function') {
    return formatPrimitive(ctx.stylize, value);
  }
  if (value === null) {
    return ctx.stylize('null', 'null');
  }

  if (ctx.showProxy) {
    const proxy = getProxyDetails(value);
    if (proxy !== undefined) {
      if (recurseTimes != null) {
        if (recurseTimes < 0)
          return ctx.stylize('Proxy [Array]', 'special');
        recurseTimes -= 1;
      }
      ctx.indentationLvl += 2;
      const res = [
        formatValue(ctx, proxy[0], recurseTimes),
        formatValue(ctx, proxy[1], recurseTimes)
      ];
      ctx.indentationLvl -= 2;
      const str = reduceToSingleString(ctx, res, '', ['[', ']']);
      return `Proxy ${str}`;
    }
  }

  // Provide a hook for user-specified inspect functions.
  // Check that value is an object with an inspect function on it
  if (ctx.customInspect) {
    const maybeCustomInspect = value[customInspectSymbol] || value.inspect;

    if (typeof maybeCustomInspect === 'function' &&
        // Filter out the util module, its inspect function is special
        maybeCustomInspect !== exports.inspect &&
        // Also filter out any prototype objects using the circular check.
        !(value.constructor && value.constructor.prototype === value)) {
      const ret = maybeCustomInspect.call(value, recurseTimes, ctx);

      // If the custom inspection method returned `this`, don't go into
      // infinite recursion.
      if (ret !== value) {
        if (typeof ret !== 'string') {
          return formatValue(ctx, ret, recurseTimes);
        }
        return ret;
      }
    }
  }

  var keys;
  var symbols = ctx.getOwnPropertySymbols(value);

  // Look up the keys of the object.
  if (ctx.showHidden) {
    keys = ctx.getOwnPropertyNames(value);
  } else {
    keys = ctx.getKeys(value);
    if (symbols.length !== 0)
      symbols = symbols.filter((key) => PropertyIsEnumerable(value, key));
  }

  const keyLength = keys.length + symbols.length;
  const constructor = getConstructorOf(value);
  let ctorName = getFunctionName(constructor);
  if (ctorName !== '')
    ctorName = `${ctorName} `;

  var base = '';
  var formatter = formatObject;
  var braces = ['{', '}'];
  var raw;
  var byteLength;
  var formatted;

  switch (getValueKind(value)) {
    case 'Array':
      // Only set the constructor for non ordinary ("Array [...]") arrays.
      braces = [`${ctorName === 'Array ' ? '' : ctorName}[`, ']'];
      if (value.length === 0 && keyLength === 0)
        return `${braces[0]}]`;
      formatter = formatArray;
      break;
    case 'Set':
      if (value.size === 0 && keyLength === 0)
        return `${ctorName}{}`;
      braces = [`${ctorName}{`, '}'];
      formatter = formatSet;
      break;
    case 'Map':
      if (value.size === 0 && keyLength === 0)
        return `${ctorName}{}`;
      braces = [`${ctorName}{`, '}'];
      formatter = formatMap;
      break;
    case 'TypedArray':
      braces = [`${ctorName}[`, ']'];
      formatter = formatTypedArray;
      break;
    case 'MapIterator':
      braces = ['MapIterator {', '}'];
      formatter = formatCollectionIterator;
      break;
    case 'SetIterator':
      braces = ['SetIterator {', '}'];
      formatter = formatCollectionIterator;
      break;
    case 'StringObject':
      raw = StringValueOf(value);

      formatted = formatPrimitive(stylizeNoColor, raw);
      if (keyLength === raw.length)
        return ctx.stylize(`[String: ${formatted}]`, 'string');
      base = ` [String: ${formatted}]`;
      // For boxed Strings, we have to remove the 0-n indexed entries,
      // since they just noisy up the output and are redundant
      // Make boxed primitive Strings look like such
      keys = keys.slice(raw.length);
      braces = ['{', '}'];
      break;
    case 'RegExp':
      // Make RegExps say that they are RegExps
      if (keyLength === 0 || recurseTimes < 0)
        return ctx.stylize(RegExpToString(value), 'regexp');
      base = ` ${RegExpToString(value)}`;
      break;
    case 'SharedArrayBuffer':
      byteLength = SharedArrayBufferByteLength(value);
      if (keyLength === 0)
        return ctorName +
              `{ byteLength: ${formatNumber(ctx.stylize, byteLength)} }`;
      braces[0] = `${ctorName}{`;
      keys.unshift(['byteLength', byteLength]);
      break;
    case 'ArrayBuffer':
      byteLength = ArrayBufferByteLength(value);
      if (keyLength === 0)
        return ctorName +
              `{ byteLength: ${formatNumber(ctx.stylize, byteLength)} }`;
      braces[0] = `${ctorName}{`;
      keys.unshift(['byteLength', byteLength]);
      break;
    case 'DataView':
      braces[0] = `${ctorName}{`;
      // .buffer goes last, it's not a primitive like the others.
      keys.unshift(['byteLength', DataViewByteLength(value)],
                   ['byteOffset', DataViewByteOffset(value)],
                   ['buffer', DataViewBuffer(value)]);
      break;
    case 'Promise':
      braces[0] = `${ctorName}{`;
      formatter = formatPromise;
      break;
    case 'Date':
      if (keyLength === 0) {
        if (Number.isNaN(DateGetTime(value)))
          return ctx.stylize(DateToString(value), 'date');
        return ctx.stylize(DateToISOString(value), 'date');
      }
      // Make dates with properties first say the date
      base = ` ${DateToISOString(value)}`;
      break;
    case 'NumberObject':
      raw = NumberValueOf(value);
      // Make boxed primitive Numbers look like such
      formatted = formatPrimitive(stylizeNoColor, raw);
      if (keyLength === 0)
        return ctx.stylize(`[Number: ${formatted}]`, 'number');
      base = ` [Number: ${formatted}]`;
      break;
    case 'BooleanObject':
      raw = BooleanValueOf(value);
      // Make boxed primitive Booleans look like such
      formatted = formatPrimitive(stylizeNoColor, raw);
      if (keyLength === 0)
        return ctx.stylize(`[Boolean: ${formatted}]`, 'boolean');
      base = ` [Boolean: ${formatted}]`;
      break;
    case 'SymbolObject':
      raw = SymbolValueOf(value);
      formatted = formatPrimitive(stylizeNoColor, raw);
      return ctx.stylize(`[Symbol: ${formatted}]`, 'symbol');
    case 'External':
      return ctx.stylize('[External]', 'special');
    default:
      if (ctorName === 'Object ') {
        // Object fast path
        if (keyLength === 0)
          return '{}';
      } else if (typeof value === 'function') {
        let valueName = getFunctionName(value);
        if (valueName !== '')
          valueName = `: ${valueName}`;
        else
          valueName = '';
        const name = `${getFunctionName(constructor)}${valueName}`;
        if (keyLength === 0)
          return ctx.stylize(`[${name}]`, 'special');
        base = ` [${name}]`;
      } else if (isError(value)) {
        // Make error with message first say the error
        if (keyLength === 0)
          return formatError(value);
        base = ` ${formatError(value)}`;
      } else if (keyLength === 0) {
        return `${ctorName}{}`;
      } else {
        braces[0] = `${ctorName}{`;
      }
      break;
  }

  // Using an array here is actually better for the average case than using
  // a Set. `seen` will only check for the depth and will never grow to large.
  if (ctx.seen.indexOf(value) !== -1)
    return ctx.stylize('[Circular]', 'special');

  if (recurseTimes != null) {
    if (recurseTimes < 0)
      return ctx.stylize(`[${constructor ? constructor.name : 'Object'}]`,
                         'special');
    recurseTimes -= 1;
  }

  ctx.seen.push(value);
  const output = formatter(ctx, value, recurseTimes, keys);

  for (var i = 0; i < symbols.length; i++) {
    output.push(formatProperty(ctx, value, recurseTimes, symbols[i], 0));
  }
  ctx.seen.pop();

  return reduceToSingleString(ctx, output, base, braces, ln);
}

function formatNumber(fn, value) {
  // Format -0 as '-0'. A `value === -0` check won't distinguish 0 from -0.
  // Using a division check is currently faster than `Object.is(value, -0)`
  // as of V8 6.1.
  if (1 / value === -Infinity)
    return fn('-0', 'number');
  return fn(`${value}`, 'number');
}

function formatPrimitive(fn, value) {
  if (typeof value === 'string')
    return fn(strEscape(value), 'string');
  if (typeof value === 'number')
    return formatNumber(fn, value);
  if (typeof value === 'boolean')
    return fn(`${value}`, 'boolean');
  if (typeof value === 'undefined')
    return fn('undefined', 'undefined');
  // es6 symbol primitive
  return fn(SymbolToString(value), 'symbol');
}

function formatError(value) {
  const stack = getProperty(value, 'stack');
  if (stack)
    return stack;
  return `[${ErrorToString(value)}]`;
}

function formatObject(ctx, value, recurseTimes, keys) {
  const len = keys.length;
  const output = ctx.createSafeArray(len);
  for (var i = 0; i < len; i++)
    output[i] = formatProperty(ctx, value, recurseTimes, keys[i], 0);
  return output;
}

// The array is sparse and/or has extra keys
function formatSpecialArray(ctx, value, recurseTimes, keys, maxLength, valLen) {
  const output = ctx.createSafeArray();
  const keyLen = keys.length;
  var visibleLength = 0;
  var i = 0;
  if (keyLen !== 0 && RegExpTest(numberRegExp, keys[0])) {
    for (const key of keys) {
      if (visibleLength === maxLength)
        break;
      const index = +key;
      // Arrays can only have up to 2^32 - 1 entries
      if (index > 2 ** 32 - 2)
        break;
      if (i !== index) {
        if (!RegExpTest(numberRegExp, key))
          break;
        const emptyItems = index - i;
        const ending = emptyItems > 1 ? 's' : '';
        const message = `<${emptyItems} empty item${ending}>`;
        output.push(ctx.stylize(message, 'undefined'));
        i = index;
        if (++visibleLength === maxLength)
          break;
      }
      output.push(formatProperty(ctx, value, recurseTimes, key, 1));
      visibleLength++;
      i++;
    }
  }
  if (i < valLen && visibleLength !== maxLength) {
    const len = valLen - i;
    const ending = len > 1 ? 's' : '';
    const message = `<${len} empty item${ending}>`;
    output.push(ctx.stylize(message, 'undefined'));
    i = valLen;
    if (keyLen === 0)
      return output;
  }
  const remaining = valLen - i;
  if (remaining > 0) {
    output.push(`... ${remaining} more item${remaining > 1 ? 's' : ''}`);
  }
  if (ctx.showHidden && keys[keyLen - 1] === 'length') {
    // No extra keys
    output.push(formatProperty(ctx, value, recurseTimes, 'length', 2));
  } else if (valLen === 0 ||
    keyLen > valLen && keys[valLen - 1] === `${valLen - 1}`) {
    // The array is not sparse
    for (i = valLen; i < keyLen; i++)
      output.push(formatProperty(ctx, value, recurseTimes, keys[i], 2));
  } else if (keys[keyLen - 1] !== `${valLen - 1}`) {
    const extra = ctx.createSafeArray();
    // Only handle special keys
    var key;
    for (i = keys.length - 1; i >= 0; i--) {
      key = keys[i];
      if (RegExpTest(numberRegExp, key) && +key < 2 ** 32 - 1)
        break;
      extra.push(formatProperty(ctx, value, recurseTimes, key, 2));
    }
    for (i = extra.length - 1; i >= 0; i--)
      output.push(extra[i]);
  }
  return output;
}

function formatArray(ctx, value, recurseTimes, keys) {
  const valLen = value.length;
  const len = min(max(0, ctx.maxArrayLength), valLen);
  const hidden = ctx.showHidden ? 1 : 0;
  const keyLen = keys.length - hidden;
  if (keyLen !== valLen || keys[keyLen - 1] !== `${valLen - 1}`)
    return formatSpecialArray(ctx, value, recurseTimes, keys, len, valLen);

  const remaining = valLen - len;
  const output = ctx.createSafeArray(len + (remaining > 0 ? 1 : 0) + hidden);
  for (var i = 0; i < len; i++)
    output[i] = formatProperty(ctx, value, recurseTimes, keys[i], 1);
  if (remaining > 0)
    output[i++] = `... ${remaining} more item${remaining > 1 ? 's' : ''}`;
  if (ctx.showHidden === true)
    output[i] = formatProperty(ctx, value, recurseTimes, 'length', 2);
  return output;
}

function formatTypedArray(ctx, value, recurseTimes, keys) {
  const valLen = TypedArrayLength(value);
  const maxLength = min(max(0, ctx.maxArrayLength), valLen);
  const remaining = valLen - maxLength;
  const output = ctx.createSafeArray(maxLength + (remaining > 0 ? 1 : 0));
  for (var i = 0; i < maxLength; ++i)
    output[i] = formatNumber(ctx.stylize, value[i]);
  if (remaining > 0)
    output[i] = `... ${remaining} more item${remaining > 1 ? 's' : ''}`;
  if (ctx.showHidden) {
    // .buffer goes last, it's not a primitive like the others.
    const extraKeys = [
      'BYTES_PER_ELEMENT',
      'length',
      'byteLength',
      'byteOffset',
      'buffer'
    ];
    for (i = 0; i < extraKeys.length; i++) {
      const str = formatValue(ctx, value[extraKeys[i]], recurseTimes);
      output.push(`[${extraKeys[i]}]: ${str}`);
    }
  }
  // TypedArrays cannot have holes. Therefore it is safe to assume that all
  // extra keys are indexed after value.length.
  for (i = valLen; i < keys.length; i++) {
    output.push(formatProperty(ctx, value, recurseTimes, keys[i], 2));
  }
  return output;
}

function formatSet(ctx, value, recurseTimes, keys) {
  const size = SetSize(value);
  const output =
    ctx.createSafeArray(size + keys.length + (ctx.showHidden ? 1 : 0));
  var i = 0;
  for (const v of ctx.getSetIterable(value))
    output[i++] = formatValue(ctx, v, recurseTimes);
  // With `showHidden`, `length` will display as a hidden property for
  // arrays. For consistency's sake, do the same for `size`, even though this
  // property isn't selected by Object.getOwnPropertyNames().
  if (ctx.showHidden)
    output[i++] = `[size]: ${ctx.stylize(`${size}`, 'number')}`;
  for (var n = 0; n < keys.length; n++) {
    output[i++] = formatProperty(ctx, value, recurseTimes, keys[n], 0);
  }
  return output;
}

function formatMap(ctx, value, recurseTimes, keys) {
  const size = MapSize(value);
  const output =
    ctx.createSafeArray(size + keys.length + (ctx.showHidden ? 1 : 0));
  var i = 0;
  for (const [k, v] of ctx.getMapIterable(value))
    output[i++] = `${formatValue(ctx, k, recurseTimes)} => ` +
      formatValue(ctx, v, recurseTimes);
  // See comment in formatSet
  if (ctx.showHidden)
    output[i++] = `[size]: ${ctx.stylize(`${size}`, 'number')}`;
  for (var n = 0; n < keys.length; n++) {
    output[i++] = formatProperty(ctx, value, recurseTimes, keys[n], 0);
  }
  return output;
}

function formatCollectionIterator(ctx, value, recurseTimes, keys) {
  if (!ctx.allowSideEffects)
    return [];
  ensureDebugIsInitialized();
  const mirror = Debug.MakeMirror(value, true);
  const vals = mirror.preview();
  const output = [];
  for (const o of vals) {
    output.push(formatValue(ctx, o, recurseTimes));
  }
  return output;
}

function formatPromise(ctx, value, recurseTimes, keys) {
  const output = ctx.createSafeArray();
  const promiseInfo = getPromiseDetails(value);
  const state = promiseInfo[0];
  const result = promiseInfo[1];
  if (state === kPending) {
    output.push('<pending>');
  } else {
    const str = formatValue(ctx, result, recurseTimes);
    output.push(state === kRejected ? `<rejected> ${str}` : str);
  }
  for (var n = 0; n < keys.length; n++) {
    output.push(formatProperty(ctx, value, recurseTimes, keys[n], 0));
  }
  return output;
}

function formatProperty(ctx, value, recurseTimes, key, array) {
  var name, str;
  let desc;

  if (typeof key === 'object') {
    desc = { value: key[1], enumerable: true };
    key = key[0];
  } else {
    desc = getPropertyDescriptor(value, key);
  }

  if (HasOwnProperty(desc, 'value')) {
    const diff = array === 0 ? 3 : 2;
    ctx.indentationLvl += diff;
    str = formatValue(ctx, desc.value, recurseTimes, array === 0);
    ctx.indentationLvl -= diff;
  } else if (HasOwnProperty(desc, 'get') && desc.get !== undefined) {
    if (HasOwnProperty(desc, 'set') && desc.set !== undefined) {
      str = ctx.stylize('[Getter/Setter]', 'special');
    } else {
      str = ctx.stylize('[Getter]', 'special');
    }
  } else if (HasOwnProperty(desc, 'set') && desc.set !== undefined) {
    str = ctx.stylize('[Setter]', 'special');
  } else {
    str = ctx.stylize('undefined', 'undefined');
  }
  if (array === 1) {
    return str;
  }
  if (typeof key === 'symbol') {
    name = `[${ctx.stylize(SymbolToString(key), 'symbol')}]`;
  } else if (desc.enumerable === false) {
    name = `[${key}]`;
  } else if (RegExpTest(keyStrRegExp, key)) {
    name = ctx.stylize(key, 'name');
  } else {
    name = ctx.stylize(keyEscape(key), 'string');
  }

  return `${name}: ${str}`;
}

function reduceToSingleString(ctx, output, base, braces, addLn) {
  const breakLength = ctx.breakLength;
  if (output.length * 2 <= breakLength) {
    var length = 0;
    for (var i = 0; i < output.length && length <= breakLength; i++) {
      if (ctx.colors) {
        length += StringReplace(output[i], colorRegExp, '').length + 1;
      } else {
        length += output[i].length + 1;
      }
    }
    if (length <= breakLength)
      return `${braces[0]}${base} ${join(output, ', ')} ${braces[1]}`;
  }
  // If the opening "brace" is too large, like in the case of "Set {",
  // we need to force the first item to be on the next line or the
  // items will not line up correctly.
  const indentation = ' '.repeat(ctx.indentationLvl);
  const extraLn = addLn === true ? `\n${indentation}` : '';
  const ln = base === '' && braces[0].length === 1 ?
    ' ' : `${base}\n${indentation}  `;
  const str = join(output, `,\n${indentation}  `);
  return `${extraLn}${braces[0]}${ln}${str} ${braces[1]}`;
}

function isBoolean(arg) {
  return typeof arg === 'boolean';
}

function isNull(arg) {
  return arg === null;
}

function isNullOrUndefined(arg) {
  return arg === null || arg === undefined;
}

function isNumber(arg) {
  return typeof arg === 'number';
}

function isString(arg) {
  return typeof arg === 'string';
}

function isSymbol(arg) {
  return typeof arg === 'symbol';
}

function isUndefined(arg) {
  return arg === undefined;
}

function isObject(arg) {
  return arg !== null && typeof arg === 'object';
}

function isFunction(arg) {
  return typeof arg === 'function';
}

function isPrimitive(arg) {
  return arg === null ||
         typeof arg !== 'object' && typeof arg !== 'function';
}

function pad(n) {
  return n < 10 ? `0${n.toString(10)}` : n.toString(10);
}

const months = ['Jan', 'Feb', 'Mar', 'Apr', 'May', 'Jun', 'Jul', 'Aug', 'Sep',
                'Oct', 'Nov', 'Dec'];

// 26 Feb 16:19:34
function timestamp() {
  var d = new Date();
  var time = [pad(d.getHours()),
              pad(d.getMinutes()),
              pad(d.getSeconds())].join(':');
  return [d.getDate(), months[d.getMonth()], time].join(' ');
}

// log is just a thin wrapper to console.log that prepends a timestamp
function log() {
  console.log('%s - %s', timestamp(), exports.format.apply(exports, arguments));
}

/**
 * Inherit the prototype methods from one constructor into another.
 *
 * The Function.prototype.inherits from lang.js rewritten as a standalone
 * function (not on Function.prototype). NOTE: If this file is to be loaded
 * during bootstrapping this function needs to be rewritten using some native
 * functions as prototype setup using normal JavaScript does not work as
 * expected during bootstrapping (see mirror.js in r114903).
 *
 * @param {function} ctor Constructor function which needs to inherit the
 *     prototype.
 * @param {function} superCtor Constructor function to inherit prototype from.
 * @throws {TypeError} Will error if either constructor is null, or if
 *     the super constructor lacks a prototype.
 */
function inherits(ctor, superCtor) {

  if (ctor === undefined || ctor === null)
    throw new errors.TypeError('ERR_INVALID_ARG_TYPE', 'ctor', 'function');

  if (superCtor === undefined || superCtor === null)
    throw new errors.TypeError('ERR_INVALID_ARG_TYPE', 'superCtor', 'function');

  if (superCtor.prototype === undefined) {
    throw new errors.TypeError('ERR_INVALID_ARG_TYPE', 'superCtor.prototype',
                               'function');
  }
  ctor.super_ = superCtor;
  Object.setPrototypeOf(ctor.prototype, superCtor.prototype);
}

function _extend(target, source) {
  // Don't do anything if source isn't an object
  if (source === null || typeof source !== 'object') return target;

  var keys = Object.keys(source);
  var i = keys.length;
  while (i--) {
    target[keys[i]] = source[keys[i]];
  }
  return target;
}

// Deprecated old stuff.

function print(...args) {
  for (var i = 0, len = args.length; i < len; ++i) {
    process.stdout.write(String(args[i]));
  }
}

function puts(...args) {
  for (var i = 0, len = args.length; i < len; ++i) {
    process.stdout.write(`${args[i]}\n`);
  }
}

function debug(x) {
  process.stderr.write(`DEBUG: ${x}\n`);
}

function error(...args) {
  for (var i = 0, len = args.length; i < len; ++i) {
    process.stderr.write(`${args[i]}\n`);
  }
}

function _errnoException(err, syscall, original) {
  if (typeof err !== 'number' || err >= 0 || !Number.isSafeInteger(err)) {
    throw new errors.TypeError('ERR_INVALID_ARG_TYPE', 'err',
                               'negative number');
  }
  const name = errname(err);
  var message = `${syscall} ${name}`;
  if (original)
    message += ` ${original}`;
  const e = new Error(message);
  e.code = e.errno = name;
  e.syscall = syscall;
  return e;
}

function _exceptionWithHostPort(err,
                                syscall,
                                address,
                                port,
                                additional) {
  var details;
  if (port && port > 0) {
    details = `${address}:${port}`;
  } else {
    details = address;
  }

  if (additional) {
    details += ` - Local (${additional})`;
  }
  var ex = exports._errnoException(err, syscall, details);
  ex.address = address;
  if (port) {
    ex.port = port;
  }
  return ex;
}

// process.versions needs a custom function as some values are lazy-evaluated.
process.versions[inspect.custom] =
  () => exports.format(JSON.parse(JSON.stringify(process.versions)));

function callbackifyOnRejected(reason, cb) {
  // `!reason` guard inspired by bluebird (Ref: https://goo.gl/t5IS6M).
  // Because `null` is a special error value in callbacks which means "no error
  // occurred", we error-wrap so the callback consumer can distinguish between
  // "the promise rejected with null" or "the promise fulfilled with undefined".
  if (!reason) {
    const newReason = new errors.Error('ERR_FALSY_VALUE_REJECTION');
    newReason.reason = reason;
    reason = newReason;
    Error.captureStackTrace(reason, callbackifyOnRejected);
  }
  return cb(reason);
}

function callbackify(original) {
  if (typeof original !== 'function') {
    throw new errors.TypeError(
      'ERR_INVALID_ARG_TYPE',
      'original',
      'function');
  }

  // We DO NOT return the promise as it gives the user a false sense that
  // the promise is actually somehow related to the callback's execution
  // and that the callback throwing will reject the promise.
  function callbackified(...args) {
    const maybeCb = args.pop();
    if (typeof maybeCb !== 'function') {
      throw new errors.TypeError(
        'ERR_INVALID_ARG_TYPE',
        'last argument',
        'function');
    }
    const cb = (...args) => { Reflect.apply(maybeCb, this, args); };
    // In true node style we process the callback on `nextTick` with all the
    // implications (stack, `uncaughtException`, `async_hooks`)
    Reflect.apply(original, this, args)
      .then((ret) => process.nextTick(cb, null, ret),
            (rej) => process.nextTick(callbackifyOnRejected, rej, cb));
  }

  Object.setPrototypeOf(callbackified, Object.getPrototypeOf(original));
  Object.defineProperties(callbackified,
                          Object.getOwnPropertyDescriptors(original));
  return callbackified;
}

// Keep the `exports =` so that various functions can still be monkeypatched
module.exports = exports = {
  _errnoException,
  _exceptionWithHostPort,
  _extend,
  callbackify,
  debuglog,
  deprecate,
  format,
  inherits,
  inspect,
  isArray,
  isBoolean,
  isBuffer,
  isDeepStrictEqual,
  isNull,
  isNullOrUndefined,
  isNumber,
  isString,
  isSymbol,
  isUndefined,
  isRegExp,
  isObject,
  isDate,
  isError,
  isFunction,
  isPrimitive,
  log,
  promisify,
  TextDecoder,
  TextEncoder,

  // Deprecated Old Stuff
  debug: deprecate(debug,
                   'util.debug is deprecated. Use console.error instead.',
                   'DEP0028'),
  error: deprecate(error,
                   'util.error is deprecated. Use console.error instead.',
                   'DEP0029'),
  print: deprecate(print,
                   'util.print is deprecated. Use console.log instead.',
                   'DEP0026'),
  puts: deprecate(puts,
                  'util.puts is deprecated. Use console.log instead.',
                  'DEP0027')
};
