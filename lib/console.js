'use strict';

const util = require('util');
const { doNotEmitErrorEvent } = require('stream').Writable;

function Console(stdout, stderr, ignoreErrors) {
  if (!(this instanceof Console)) {
    return new Console(stdout, stderr, ignoreErrors);
  }
  if (!stdout || typeof stdout.write !== 'function') {
    throw new TypeError('Console expects a writable stream instance');
  }
  if (!stderr) {
    stderr = stdout;
  } else if (typeof stderr.write !== 'function') {
    throw new TypeError('Console expects writable stream instances');
  }

  var prop = {
    writable: true,
    enumerable: false,
    configurable: true
  };
  prop.value = stdout;
  Object.defineProperty(this, '_stdout', prop);
  prop.value = stderr;
  Object.defineProperty(this, '_stderr', prop);
  prop.value = ignoreErrors === undefined ? true : !!ignoreErrors;
  Object.defineProperty(this, '_ignoreErrors', prop);
  prop.value = new Map();
  Object.defineProperty(this, '_times', prop);

  // bind the prototype functions to this Console instance
  var keys = Object.keys(Console.prototype);
  for (var v = 0; v < keys.length; v++) {
    var k = keys[v];
    this[k] = this[k].bind(this);
  }
}

function write(ignoreErrors, stream, string) {
  if (!ignoreErrors) return stream.write(string);

  try {
    stream.write(string, (err) => {
      return doNotEmitErrorEvent;
    });
  } catch (e) {
    // Sorry, there’s proper way to pass along the error here.
  }
}


// As of v8 5.0.71.32, the combination of rest param, template string
// and .apply(null, args) benchmarks consistently faster than using
// the spread operator when calling util.format.
Console.prototype.log = function log(...args) {
  write(this._ignoreErrors, this._stdout, `${util.format.apply(null, args)}\n`);
};


Console.prototype.info = Console.prototype.log;


Console.prototype.warn = function warn(...args) {
  write(this._ignoreErrors, this._stderr, `${util.format.apply(null, args)}\n`);
};


Console.prototype.error = Console.prototype.warn;


Console.prototype.dir = function dir(object, options) {
  options = Object.assign({customInspect: false}, options);
  write(this._ignoreErrors, this._stdout, `${util.inspect(object, options)}\n`);
};


Console.prototype.time = function time(label) {
  this._times.set(label, process.hrtime());
};


Console.prototype.timeEnd = function timeEnd(label) {
  const time = this._times.get(label);
  if (!time) {
    process.emitWarning(`No such label '${label}' for console.timeEnd()`);
    return;
  }
  const duration = process.hrtime(time);
  const ms = duration[0] * 1000 + duration[1] / 1e6;
  this.log('%s: %sms', label, ms.toFixed(3));
  this._times.delete(label);
};


Console.prototype.trace = function trace(...args) {
  // TODO probably can to do this better with V8's debug object once that is
  // exposed.
  var err = new Error();
  err.name = 'Trace';
  err.message = util.format.apply(null, args);
  Error.captureStackTrace(err, trace);
  this.error(err.stack);
};


Console.prototype.assert = function assert(expression, ...args) {
  if (!expression) {
    require('assert').ok(false, util.format.apply(null, args));
  }
};


module.exports = new Console(process.stdout, process.stderr);
module.exports.Console = Console;
