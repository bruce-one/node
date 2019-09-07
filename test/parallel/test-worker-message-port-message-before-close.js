'use strict';
const common = require('../common');
const assert = require('assert');
const { once } = require('events');
const { Worker, MessageChannel } = require('worker_threads');

// This is a regression test for the race condition underlying
// https://github.com/nodejs/node/issues/22762.
// It ensures that all messages send before a MessagePort#close() call are
// received. Previously, what could happen was a race condition like this:
// - Thread 1 sends message A
// - Thread 2 begins receiving/emitting message A
// - Thread 1 sends message B
// - Thread 1 closes its side of the channel
// - Thread 2 finishes receiving/emitting message A
// - Thread 2 sees that the port should be closed
// - Thread 2 closes the port, discarding message B in the process.

const base = process.hrtime.bigint()
const logs = [];
async function test() {
  const worker = new Worker(`
  require('worker_threads').parentPort.on('message', ({ port }) => {
    port.postMessage('firstMessage');
    port.postMessage('lastMessage');
    port.close();
  });
  `, { eval: true });
  worker.unref();

  for (let i = 0; i < 10000; i++) {
    const { port1, port2 } = new MessageChannel();
    worker.postMessage({ port: port2 }, [ port2 ]);
    assert.deepStrictEqual(await once(port1, 'message'), ['firstMessage']);
    logs.push([i, 'got firstMessage', process.hrtime.bigint() - base]);
    assert.deepStrictEqual(await once(port1, 'message'), ['lastMessage']);
    logs.push([i, 'got lastMessage', process.hrtime.bigint() - base]);
  }

  await worker.terminate();
  logs.push([-1, 'done', process.hrtime.bigint() - base]);
}

test().then(common.mustCall());

setTimeout(() => {
  console.log(logs.join('\n'))
}, 10000).unref();
