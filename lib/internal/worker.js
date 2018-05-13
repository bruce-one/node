'use strict';

const Buffer = require('buffer').Buffer;
const EventEmitter = require('events');
const assert = require('assert');
const path = require('path');
const util = require('util');
const { Readable, Writable } = require('stream');
const {
  ERR_INVALID_ARG_TYPE,
  ERR_WORKER_NEED_ABSOLUTE_PATH,
  ERR_WORKER_UNSERIALIZABLE_ERROR,
  ERR_WORKER_UNSUPPORTED_EXTENSION,
} = require('internal/errors').codes;

const { internalBinding } = require('internal/bootstrap/loaders');
const { MessagePort, MessageChannel } = internalBinding('messaging');
const { clearAsyncIdStack } = require('internal/async_hooks');

util.inherits(MessagePort, EventEmitter);

const {
  Worker: WorkerImpl,
  getEnvMessagePort,
  threadId,
  kMessageFlagNone,
  kMessageFlagCustomOffset
} = internalBinding('worker');

const isMainThread = threadId === 0;

const kOnFlaggedMessage = Symbol('kOnFlaggedMessage');
const kOnMessageListener = Symbol('kOnMessageListener');
const kHandle = Symbol('kHandle');
const kName = Symbol('kName');
const kPort = Symbol('kPort');
const kDispose = Symbol('kDispose');
const kOnExit = Symbol('kOnExit');
const kOnMessage = Symbol('kOnMessage');
const kOnCouldNotSerializeErr = Symbol('kOnCouldNotSerializeErr');
const kOnErrorMessage = Symbol('kOnErrorMessage');
const kParentSideStdio = Symbol('kParentSideStdio');
const kWritableCallback = Symbol('kWritableCallback');
const kStdioWantsMoreDataCallback = Symbol('kStdioWantsMoreDataCallback');

const debug = util.debuglog('worker');

// Flags >= kMessageFlagCustomOffset are reserved for custom commands in the
// sense that they are not handled in the C++ layer.
// kMessageFlagNone is the default flag for normal user messages sent by
// `postMessage`.
const kUpAndRunning = kMessageFlagCustomOffset;
const kLoadScript = kMessageFlagCustomOffset + 1;
const kErrorMessage = kMessageFlagCustomOffset + 2;
const kCouldNotSerializeError = kMessageFlagCustomOffset + 3;
const kStdioPayload = kMessageFlagCustomOffset + 4;
const kStdioWantsMoreData = kMessageFlagCustomOffset + 5;

// A communication channel consisting of a handle (that wraps around an
// uv_async_t) which can receive information from other threads and emits
// .onmessage events, and a function used for sending data to a MessagePort
// in some other thread.
MessagePort.prototype[kOnMessageListener] = function onmessage(payload, flag) {
  debug(`[${threadId}] received message`, flag, payload);
  // Emit the flag and deserialized object to userland.
  if (flag === 0 || flag === undefined)
    this.emit('message', payload);
  else
    this.emit(kOnFlaggedMessage, flag, payload);
};

// This is for compatibility with the Web's MessagePort API. It makes sense to
// provide it as an `EventEmitter` in Node.js, but if somebody overrides
// `onmessage`, we'll switch over to the Web API model.
Object.defineProperty(MessagePort.prototype, 'onmessage', {
  enumerable: true,
  configurable: true,
  get() {
    return this[kOnMessageListener];
  },
  set(value) {
    this[kOnMessageListener] = value;
    if (typeof value === 'function') {
      this.ref();
      this.start();
    } else {
      this.unref();
      this.stop();
    }
  }
});

function oninit() {
  setupPortReferencing(this, this, 'message');
}

Object.defineProperty(MessagePort.prototype, 'oninit', {
  enumerable: true,
  writable: false,
  value: oninit
});

function onclose() {
  this.emit('close');
}

Object.defineProperty(MessagePort.prototype, 'onclose', {
  enumerable: true,
  writable: false,
  value: onclose
});

function setupPortReferencing(port, eventEmitter, eventName) {
  // Keep track of whether there are any workerMessage listeners:
  // If there are some, ref() the channel so it keeps the event loop alive.
  // If there are none or all are removed, unref() the channel so the worker
  // can shutdown gracefully.
  port.unref();
  eventEmitter.on('newListener', (name) => {
    if (name === eventName && eventEmitter.listenerCount(eventName) === 0) {
      port.ref();
      port.start();
    }
  });
  eventEmitter.on('removeListener', (name) => {
    if (name === eventName && eventEmitter.listenerCount(eventName) === 0) {
      port.stop();
      port.unref();
    }
  });
}


class ReadableWorkerStdio extends Readable {
  constructor(port, name) {
    super();
    this[kPort] = port;
    this[kName] = name;
  }

  _read() {
    this[kPort].postMessage({ stream: this[kName] },
                            undefined,
                            kStdioWantsMoreData);
  }
}

class WritableWorkerStdio extends Writable {
  constructor(port, name) {
    super({ decodeStrings: false });
    this[kPort] = port;
    this[kName] = name;
    this[kWritableCallback] = noop;
  }

  _write(chunk, encoding, cb) {
    this[kPort].postMessage({ chunk, encoding, stream: this[kName] },
                            undefined,
                            kStdioPayload);
    this[kWritableCallback] = cb;
  }

  _flush(cb) {
    this[kPort].postMessage({ chunk: null, stream: this[kName] },
                            undefined,
                            kStdioPayload);
  }

  [kStdioWantsMoreDataCallback]() {
    this[kWritableCallback]();
    this[kWritableCallback] = noop;
  }
}

function noop() {}


class Worker extends EventEmitter {
  constructor(filename, options = {}) {
    super();
    debug(`[${threadId}] create new worker`, filename, options);
    if (typeof filename !== 'string') {
      throw new ERR_INVALID_ARG_TYPE('filename', 'string', filename);
    }

    if (!options.eval) {
      if (!path.isAbsolute(filename)) {
        throw new ERR_WORKER_NEED_ABSOLUTE_PATH(filename);
      }
      const ext = path.extname(filename);
      if (ext !== '.js' && ext !== '.mjs') {
        throw new ERR_WORKER_UNSUPPORTED_EXTENSION(ext);
      }
    }

    // Set up the C++ handle for the worker, as well as some internal wiring.
    this[kHandle] = new WorkerImpl();
    this[kHandle].onexit = (code) => this[kOnExit](code);
    this[kPort] = this[kHandle].messagePort;
    this[kPort].on('message', (payload) => this.emit('message', payload));
    this[kPort].on(kOnFlaggedMessage, (flag, payload) => {
      this[kOnMessage](flag, payload);
    });
    this[kPort].start();
    debug(`[${threadId}] created Worker with ID ${this.threadId}`);

    let stdin = null;
    if (options.stdin)
      stdin = new WritableWorkerStdio(this[kPort], 'stdin');
    const stdout = new ReadableWorkerStdio(this[kPort], 'stdout');
    if (!options.stdout)
      pipeWithoutWarning(stdout, process.stdout);
    const stderr = new ReadableWorkerStdio(this[kPort], 'stderr');
    if (!options.stderr)
      pipeWithoutWarning(stderr, process.stderr);

    this[kParentSideStdio] = { stdin, stdout, stderr };

    this[kPort].postMessage([
      filename, !!options.eval, options.workerData, !!options.stdin
    ], undefined, kLoadScript);
    // Actually start the new thread now that everything is in place.
    this[kHandle].startThread();
  }

  [kOnExit](code) {
    debug(`[${threadId}] hears end event for Worker ${this.threadId}`);
    this[kDispose]();
    this.emit('exit', code);
    this.removeAllListeners();
  }

  [kOnCouldNotSerializeErr]() {
    this.emit('error', new ERR_WORKER_UNSERIALIZABLE_ERROR());
  }

  [kOnErrorMessage](serialized) {
    // This is what is called for uncaught exceptions.
    const error = deserializeError(serialized);
    this.emit('error', error);
  }

  [kOnMessage](flag, payload) {
    switch (flag) {
      case kMessageFlagNone:
        // This is a userland message.
        return this.emit('message', payload);
      case kUpAndRunning:
        return this.emit('online');
      case kCouldNotSerializeError:
        return this[kOnCouldNotSerializeErr](payload);
      case kErrorMessage:
        return this[kOnErrorMessage](payload);
      case kStdioPayload:
      {
        const { stream, chunk, encoding } = payload;
        return this[kParentSideStdio][stream].push(chunk, encoding);
      }
      case kStdioWantsMoreData:
      {
        const { stream } = payload;
        return this[kParentSideStdio][stream][kStdioWantsMoreDataCallback]();
      }
    }

    assert.fail(`Unknown worker message flag ${flag}`);
  }

  [kDispose]() {
    this[kHandle].onexit = null;
    this[kHandle] = null;
    this[kPort] = null;
  }

  postMessage(payload, transferList = undefined) {
    if (this[kHandle] === null) return;

    this[kPort].postMessage(payload, transferList);
  }

  terminate(callback) {
    if (this[kHandle] === null) return;

    debug(`[${threadId}] terminates Worker with ID ${this.threadId}`);

    if (typeof callback !== 'undefined')
      this.once('exit', (exitCode) => callback(null, exitCode));

    this[kHandle].stopThread();
  }

  ref() {
    if (this[kPort] === null) return;

    this[kPort].ref();
  }

  unref() {
    if (this[kPort] === null) return;

    this[kPort].unref();
  }

  get threadId() {
    if (this[kHandle] === null) return -1;

    return this[kHandle].threadId;
  }

  get stdin() {
    return this[kParentSideStdio].stdin;
  }

  get stdout() {
    return this[kParentSideStdio].stdout;
  }

  get stderr() {
    return this[kParentSideStdio].stderr;
  }
}

const workerStdio = {};
if (!isMainThread) {
  const port = getEnvMessagePort();
  workerStdio.stdin = new ReadableWorkerStdio(port, 'stdin');
  workerStdio.stdout = new WritableWorkerStdio(port, 'stdout');
  workerStdio.stderr = new WritableWorkerStdio(port, 'stderr');
}

let originalFatalException;

function setupChild(evalScript) {
  // Called during bootstrap to set up worker script execution.
  debug(`[${threadId}] is setting up worker child environment`);
  const port = getEnvMessagePort();
  const publicWorker = require('worker');
  assert.strictEqual(typeof publicWorker.on, 'function');
  publicWorker.postMessage = (payload, transferList) => {
    port.postMessage(payload, transferList);
  };

  port.on('message', (payload) => publicWorker.emit('workerMessage', payload));
  port.on(kOnFlaggedMessage, (flag, payload) => {
    if (flag === kLoadScript) {
      const [ filename, doEval, workerData, hasStdin ] = payload;
      debug(`[${threadId}] starts worker script ${filename} ` +
            `(eval = ${!!doEval}) at cwd = ${process.cwd()}`);
      publicWorker.workerData = workerData;
      setupPortReferencing(port, publicWorker, 'workerMessage');
      if (!hasStdin)
        workerStdio.stdin.push(null);
      port.postMessage(null, undefined, kUpAndRunning);
      if (doEval) {
        evalScript('[worker eval]', filename);
      } else {
        process.argv[1] = filename; // script filename
        require('module').runMain();
      }
      return;
    } else if (flag === kStdioPayload) {
      const { stream, chunk, encoding } = payload;
      workerStdio[stream].push(chunk, encoding);
      return;
    } else if (flag === kStdioWantsMoreData) {
      const { stream } = payload;
      workerStdio[stream][kStdioWantsMoreDataCallback]();
      return;
    }

    assert.fail(`Unknown worker message flag ${flag}`);
  });

  port.start();

  originalFatalException = process._fatalException;
  process._fatalException = fatalException;

  function fatalException(error) {
    debug(`[${threadId}] gets fatal exception`);
    let caught = false;
    try {
      caught = originalFatalException.call(this, error);
    } catch (e) {
      error = e;
    }
    debug(`[${threadId}] fatal exception caught = ${caught}`);

    if (!caught) {
      let serialized;
      try {
        serialized = serializeError(error);
      } catch {}
      debug(`[${threadId}] fatal exception serialized = ${!!serialized}`);
      if (serialized)
        port.postMessage(serialized, undefined, kErrorMessage);
      else
        port.postMessage(0, undefined, kCouldNotSerializeError);
      clearAsyncIdStack();
    }
  }
}

// TODO(addaleax): These can be improved a lot.
function serializeError(error) {
  return Buffer.from(util.inspect(error), 'utf8');
}

function deserializeError(error) {
  return error.toString('utf8');
}

function pipeWithoutWarning(source, dest) {
  const sourceMaxListeners = source._maxListeners;
  const destMaxListeners = dest._maxListeners;
  source.setMaxListeners(Infinity);
  dest.setMaxListeners(Infinity);

  source.pipe(dest);

  source._maxListeners = sourceMaxListeners;
  dest._maxListeners = destMaxListeners;
}

module.exports = {
  MessagePort,
  MessageChannel,
  threadId,
  Worker,
  setupChild,
  isMainThread,
  workerStdio
};
