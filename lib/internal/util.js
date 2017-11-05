'use strict';

const errors = require('internal/errors');
const { signals } = process.binding('constants').os;

const {
  createPromise,
  getHiddenValue,
  promiseResolve,
  promiseReject,
  setHiddenValue,
  arrow_message_private_symbol: kArrowMessagePrivateSymbolIndex,
  decorated_private_symbol: kDecoratedPrivateSymbolIndex
} = process.binding('util');

const noCrypto = !process.versions.openssl;

const objectToString =
    Function.prototype.call.bind(Object.prototype.toString);

function isError(e) {
  return objectToString(e) === '[object Error]' || e instanceof Error;
}

// Mark that a method should not be used.
// Returns a modified function which warns once by default.
// If --no-deprecation is set, then it is a no-op.
function deprecate(fn, msg, code) {
  // Allow for deprecating things in the process of starting up.
  if (global.process === undefined) {
    return function(...args) {
      return deprecate(fn, msg).apply(this, args);
    };
  }

  if (process.noDeprecation === true) {
    return fn;
  }

  if (code !== undefined && typeof code !== 'string')
    throw new errors.TypeError('ERR_INVALID_ARG_TYPE', 'code', 'string');

  var warned = false;
  function deprecated(...args) {
    if (!warned) {
      warned = true;
      if (code !== undefined) {
        process.emitWarning(msg, 'DeprecationWarning', code, deprecated);
      } else {
        process.emitWarning(msg, 'DeprecationWarning', deprecated);
      }
    }
    if (new.target) {
      return Reflect.construct(fn, args, new.target);
    }
    return fn.apply(this, args);
  }

  // The wrapper will keep the same prototype as fn to maintain prototype chain
  Object.setPrototypeOf(deprecated, fn);
  if (fn.prototype) {
    // Setting this (rather than using Object.setPrototype, as above) ensures
    // that calling the unwrapped constructor gives an instanceof the wrapped
    // constructor.
    deprecated.prototype = fn.prototype;
  }

  return deprecated;
}

function decorateErrorStack(err) {
  if (!(isError(err) && err.stack) ||
      getHiddenValue(err, kDecoratedPrivateSymbolIndex) === true)
    return;

  const arrow = getHiddenValue(err, kArrowMessagePrivateSymbolIndex);

  if (arrow) {
    err.stack = arrow + err.stack;
    setHiddenValue(err, kDecoratedPrivateSymbolIndex, true);
  }
}

function assertCrypto() {
  if (noCrypto)
    throw new errors.Error('ERR_NO_CRYPTO');
}

// The loop should only run at most twice, retrying with lowercased enc
// if there is no match in the first pass.
// We use a loop instead of branching to retry with a helper
// function in order to avoid the performance hit.
// Return undefined if there is no match.
function normalizeEncoding(enc) {
  if (!enc) return 'utf8';
  var retried;
  while (true) {
    switch (enc) {
      case 'utf8':
      case 'utf-8':
        return 'utf8';
      case 'ucs2':
      case 'ucs-2':
      case 'utf16le':
      case 'utf-16le':
        return 'utf16le';
      case 'latin1':
      case 'binary':
        return 'latin1';
      case 'base64':
      case 'ascii':
      case 'hex':
        return enc;
      default:
        if (retried) return; // undefined
        enc = ('' + enc).toLowerCase();
        retried = true;
    }
  }
}

function filterDuplicateStrings(items, low) {
  const map = new Map();
  for (var i = 0; i < items.length; i++) {
    const item = items[i];
    const key = item.toLowerCase();
    if (low) {
      map.set(key, key);
    } else {
      map.set(key, item);
    }
  }
  return Array.from(map.values()).sort();
}

function cachedResult(fn) {
  var result;
  return () => {
    if (result === undefined)
      result = fn();
    return result.slice();
  };
}

// Useful for Wrapping an ES6 Class with a constructor Function that
// does not require the new keyword. For instance:
//   class A { constructor(x) {this.x = x;}}
//   const B = createClassWrapper(A);
//   B() instanceof A // true
//   B() instanceof B // true
function createClassWrapper(type) {
  function fn(...args) {
    return Reflect.construct(type, args, new.target || type);
  }
  // Mask the wrapper function name and length values
  Object.defineProperties(fn, {
    name: { value: type.name },
    length: { value: type.length }
  });
  Object.setPrototypeOf(fn, type);
  fn.prototype = type.prototype;
  return fn;
}

let signalsToNamesMapping;
function getSignalsToNamesMapping() {
  if (signalsToNamesMapping !== undefined)
    return signalsToNamesMapping;

  signalsToNamesMapping = Object.create(null);
  for (var key in signals) {
    signalsToNamesMapping[signals[key]] = key;
  }

  return signalsToNamesMapping;
}

function convertToValidSignal(signal) {
  if (typeof signal === 'number' && getSignalsToNamesMapping()[signal])
    return signal;

  if (typeof signal === 'string') {
    const signalName = signals[signal.toUpperCase()];
    if (signalName) return signalName;
  }

  throw new errors.TypeError('ERR_UNKNOWN_SIGNAL', signal);
}

const {
  getOwnPropertyDescriptor,
  getPrototypeOf,
  hasOwnProperty
} = Object;

const HasOwnProperty = Function.prototype.call.bind(hasOwnProperty);

function getPropertyDescriptor(value, name) {
  if (value === null || value === undefined)
    return undefined;

  const own = getOwnPropertyDescriptor(value, name);
  if (own !== undefined)
    return own;

  const proto = getPrototypeOf(value);
  if (proto === null)
    return undefined;

  return getPropertyDescriptor(proto, name);
}

function getProperty(value, name) {
  const desc = getPropertyDescriptor(value, name);
  if (desc !== undefined && HasOwnProperty(desc, 'value'))
    return desc.value;
}

function getFunctionName(fn) {
  return getProperty(fn, 'name') || '';
}

function getConstructorOf(obj) {
  while (obj) {
    const desc = getPropertyDescriptor(obj, 'constructor');
    if (desc !== undefined &&
        HasOwnProperty(desc, 'value') &&
        typeof desc.value === 'function' &&
        getFunctionName(desc.value) !== '') {
      return desc.value;
    }

    obj = getPrototypeOf(obj);
  }

  return null;
}

const kCustomPromisifiedSymbol = Symbol('util.promisify.custom');
const kCustomPromisifyArgsSymbol = Symbol('customPromisifyArgs');

function promisify(original) {
  if (typeof original !== 'function')
    throw new errors.TypeError('ERR_INVALID_ARG_TYPE', 'original', 'function');

  if (original[kCustomPromisifiedSymbol]) {
    const fn = original[kCustomPromisifiedSymbol];
    if (typeof fn !== 'function') {
      throw new errors.TypeError('ERR_INVALID_ARG_TYPE',
                                 'util.promisify.custom',
                                 'function',
                                 fn);
    }
    Object.defineProperty(fn, kCustomPromisifiedSymbol, {
      value: fn, enumerable: false, writable: false, configurable: true
    });
    return fn;
  }

  // Names to create an object from in case the callback receives multiple
  // arguments, e.g. ['stdout', 'stderr'] for child_process.exec.
  const argumentNames = original[kCustomPromisifyArgsSymbol];

  function fn(...args) {
    const promise = createPromise();
    try {
      original.call(this, ...args, (err, ...values) => {
        if (err) {
          promiseReject(promise, err);
        } else if (argumentNames !== undefined && values.length > 1) {
          const obj = {};
          for (var i = 0; i < argumentNames.length; i++)
            obj[argumentNames[i]] = values[i];
          promiseResolve(promise, obj);
        } else {
          promiseResolve(promise, values[0]);
        }
      });
    } catch (err) {
      promiseReject(promise, err);
    }
    return promise;
  }

  Object.setPrototypeOf(fn, Object.getPrototypeOf(original));

  Object.defineProperty(fn, kCustomPromisifiedSymbol, {
    value: fn, enumerable: false, writable: false, configurable: true
  });
  return Object.defineProperties(
    fn,
    Object.getOwnPropertyDescriptors(original)
  );
}

promisify.custom = kCustomPromisifiedSymbol;

// The build-in Array#join is slower in v8 6.0
function join(output, separator) {
  var str = '';
  if (output.length !== 0) {
    for (var i = 0; i < output.length - 1; i++) {
      // It is faster not to use a template string here
      str += output[i];
      str += separator;
    }
    str += output[i];
  }
  return str;
}

// About 1.5x faster than the two-arg version of Array#splice().
function spliceOne(list, index) {
  for (var i = index, k = i + 1, n = list.length; k < n; i += 1, k += 1)
    list[i] = list[k];
  list.pop();
}

module.exports = {
  assertCrypto,
  cachedResult,
  convertToValidSignal,
  createClassWrapper,
  decorateErrorStack,
  deprecate,
  filterDuplicateStrings,
  getProperty,
  getPropertyDescriptor,
  getFunctionName,
  getConstructorOf,
  isError,
  join,
  normalizeEncoding,
  objectToString,
  promisify,
  spliceOne,

  // Symbol used to customize promisify conversion
  customPromisifyArgs: kCustomPromisifyArgsSymbol,

  // Symbol used to provide a custom inspect function for an object as an
  // alternative to using 'inspect'
  customInspectSymbol: Symbol('util.inspect.custom'),

  // Used by the buffer module to capture an internal reference to the
  // default isEncoding implementation, just in case userland overrides it.
  kIsEncodingSymbol: Symbol('node.isEncoding')
};
