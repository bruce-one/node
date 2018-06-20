'use strict';
// Flags: --expose-gc --experimental-worker
/* eslint-disable node-core/no-unescaped-regexp-dot */
const common = require('../common');
const assert = require('assert');
const worker = require('worker_threads');

{
  // Check that waiting on a SAB where only one thread has access fails.
  const a = new SharedArrayBuffer(4);
  const b = new Int32Array(a);
  assert.throws(() => Atomics.wait(b, 0, 0),
                /^Error: Atomics.wait .+ is unwakeable/);
}

{
  // Check that waiting on a SAB does not throw when the values don't match.
  const a = new SharedArrayBuffer(4);
  const b = new Int32Array(a);
  assert.strictEqual(Atomics.wait(b, 0, 1), 'not-equal');
}

{
  // Check that multiple threads waiting on the same SAB fails if no other
  // isolate is available to wake them.
  const onerror = common.mustCall((err) => {
    // Do not use expectError or similar because `err` may be
    // (de-)serialized here.
    assert.ok(/Atomics.wait .+ is unwakeable/.test(err.message),
              err.message);
  }, 2);

  const a = new SharedArrayBuffer(4);
  const w = new worker.Worker(`
    const worker = require('worker_threads');
    worker.parentPort.once('message', (msg) => {
      const b = new Int32Array(msg);
      Atomics.wait(b, 0, 0);
    });
  `, { eval: true });
  w.once('error', onerror);
  w.postMessage(a);
  const b = new Int32Array(a);
  try {
    Atomics.wait(b, 0, 0);
  } catch (e) {
    onerror(e);
  }
}

{
  // Check that N threads can wait on an SAB without issues if there is a
  // (N+1)st one to wake them.
  let a = new SharedArrayBuffer(4);
  for (let i = 0; i < 5; i++) {
    const w = new worker.Worker(`
      const worker = require('worker_threads');
      worker.parentPort.once('message', (msg) => {
        const b = new Int32Array(msg);
        Atomics.wait(b, 0, 0);
      });
    `, { eval: true });
    w.postMessage(a);
  }

  {
    const w = new worker.Worker(`
      const worker = require('worker_threads');
      worker.parentPort.once('message', (msg) => {
        const b = new Int32Array(msg);
        Atomics.store(b, 0, 1);
        Atomics.notify(b, 0);
        setImmediate(global.gc);
      });
    `, { eval: true });
    w.postMessage(a);
  }
  a = null;
  global.gc();
}

{
  // Check that waiting on a SAB fails  if the other accessing thread
  // loses its access through garbage collection.
  const a = new SharedArrayBuffer(4);
  const w = new worker.Worker(`
    const worker = require('worker_threads');
    worker.parentPort.once('message', (msg) => {
      worker.parentPort.postMessage(msg);
      msg = null;
      setImmediate(global.gc);
    });
  `, { eval: true });
  w.postMessage(a);
  w.on('message', common.mustCall((a) => {
    const b = new Int32Array(a);
    assert.throws(() => Atomics.wait(b, 0, 0),
                  /^Error: Atomics.wait .+ is unwakeable/);
  }));
}

{
  // Similar to the case above, except here the parent thread loses access.
  let a = new SharedArrayBuffer(4);
  const w = new worker.Worker(`
    const worker = require('worker_threads');
    const assert = require('assert');
    worker.parentPort.once('message', (a) => {
      const b = new Int32Array(a);
      assert.throws(() => Atomics.wait(b, 0, 0),
                    /^Error: Atomics.wait .+ is unwakeable/);
    });
  `, { eval: true });
  w.postMessage(a);
  a = null;
  setImmediate(global.gc);
}

{
  // Checks Atomics.wait()/Atomics.notify() works under normal circumstances.
  // XXX Problem: Cannot tell whether thread 0 is about to be woken up, even if
  // it is the only one with access to an SAB
  const a = new SharedArrayBuffer(8);
  const b = new Int32Array(a);
  b[1] = 12345;
  const w = new worker.Worker(`
    const worker = require('worker_threads');
    const assert = require('assert');
    worker.parentPort.once('message', (b) => {
      assert.strictEqual(Atomics.load(b, 1), 12345);
      worker.parentPort.postMessage('sleeping');
      Atomics.wait(b, 0, 0);
      assert.strictEqual(Atomics.load(b, 1), 54321);
    });
  `, { eval: true });
  w.postMessage(b);
  w.on('exit', common.mustCall());
  w.on('message', common.mustCall((msg) => {
    assert.strictEqual(msg, 'sleeping');
    Atomics.store(b, 1, 54321);
    Atomics.store(b, 0, 1);
    Atomics.notify(b, 0);
  }));
}
