'use strict';
const { Readable, Writable } = require('stream');
const { owner_symbol } = require('internal/async_hooks').symbols;

const kFastpipeSupported = Symbol('kFastpipeSupported');
const kFastpipeWritableBailoutMethods = {};
const kFastpipeWritableBailoutMethodsOriginals = {};
const kFastpipeReadableBailoutMethods = {};
const kFastpipeReadableBailoutMethodsOriginals = {};

for (const name of ['cork', 'uncork', 'write', 'destroy']) {
  const original = Writable.prototype[name];
  kFastpipeWritableBailoutMethodsOriginals[name] = original;
  kFastpipeWritableBailoutMethods[name] = function(...args) {
    bailoutFromWritable(this);
    return original.call(this, ...args);
  };
}

for (const name of [
  'pause', 'resume', 'setEncoding', 'unshift', 'push', 'destroy',
  'pipe', 'unpipe'
]) {
  const original = Readable.prototype[name];
  kFastpipeReadableBailoutMethodsOriginals[name] = original;
  kFastpipeReadableBailoutMethods[name] = function(...args) {
    bailoutFromReadable(this);
    return original.call(this, ...args);
  };
}

const OriginalPipe = Readable.prototype.pipe;

function installFastpipe(StreamClass) {
  StreamClass.prototype[kFastpipeSupported] = true;
  StreamClass.prototype.pipe = function(dest, opts) {
    // If one of the streams is already part of a pipe, we first need to unpipe
    // those streams. This will call the original `.pipe()` function, which
    // adds `data` listeners, which prevents a new fastpipe from being formed
    // later here.
    if (dest._handle && dest._handle.pipeSource)
      bailoutFromWritable(dest);

    if (!dest[kFastpipeSupported] ||
        this.listenerCount('data') > 0 ||
        this.listenerCount('readable') > 0 ||
        dest.listenerCount('drain') > 0 ||
        this._readableState.decoder ||
        this._readableState.objectMode ||
        (dest._writableState && dest._writableState.objectMode)) {
      return OriginalPipe.call(this, dest, opts);
    }

    if (this.connecting) {
      OriginalPipe.call(this, dest, opts);
      this.once('connect', () => {
        this.unpipe(dest);
        this.pipe(dest, opts);
      });
      return this;
    }

    if (this._readableState.length > 0) {
      const data = this.read();
      dest.write(data);
    }

    if (dest._writableState &&
        (dest._writableState.corked || dest._writableState.writing)) {
      OriginalPipe.call(this, dest, opts);
      dest.once('drain', () => {
        this.unpipe(dest);
        this.pipe(dest, opts);
      });
      return this;
    }

    Object.assign(dest, kFastpipeWritableBailoutMethods);
    dest.on('newListener', writableNewListener);

    Object.assign(this, kFastpipeReadableBailoutMethods);
    this.on('newListener', readableNewListener);

    const { internalBinding } = require('internal/bootstrap/loaders');
    const { StreamPipe } = internalBinding('stream_pipe');
    const pipe = new StreamPipe(this._handle._externalStream,
                                dest._handle._externalStream);
    pipe.onunpipe = onunpipe;
    pipe.start();
    return this;
  };
}

function onunpipe() {
  const self = this.source[owner_symbol];
  bailoutFromReadable(self);
}

function writableNewListener(name) {
  if (name === 'drain')
    bailoutFromWritable(this);
}

function readableNewListener(name) {
  if (name === 'data' || name === 'readable')
    bailoutFromReadable(this);
}

function bailoutFromReadable(self) {
  if (!self._handle)
    return;
  const pipe = self._handle.pipeTarget;
  const dest = pipe.sink[owner_symbol];
  self.removeListener('newListener', readableNewListener);
  Object.assign(self, kFastpipeReadableBailoutMethodsOriginals);
  dest.removeListener('newListener', writableNewListener);
  Object.assign(dest, kFastpipeWritableBailoutMethodsOriginals);
  pipe.unpipe();
  pipe.onunpipe = function() {};
  if (pipe.pendingWrites() > 0) {
    pipe.oncomplete = function() {
      OriginalPipe.call(self, dest);
    };
  } else {
    OriginalPipe.call(self, dest);
  }
}

function bailoutFromWritable(dest) {
  bailoutFromReadable(dest._handle.pipeSource.source[owner_symbol]);
}

module.exports = {
  installFastpipe
};
