'use strict';

const common = require('../common');
if (!common.hasCrypto)
  common.skip('missing crypto');

const assert = require('assert');
const async_hooks = require('async_hooks');
const call_log = [0, 0, 0, 0];  // [before, callback, exception, after];
let call_id = null;
let hooks = null;

process._rawDebug(`Adding beforeExitHandler`)
process.on('beforeExit', common.mustCall(() => {
  process._rawDebug(`beforeExit`, (new Error()).stack)
  //process.removeAllListeners('uncaughtException');
  hooks.disable();
  assert.strictEqual(typeof call_id, 'number');
  assert.deepStrictEqual(call_log, [1, 1, 1, 1]);
}));


hooks = async_hooks.createHook({
  init(id, type) {
    process._rawDebug('init', id, type)
    if (type === 'RANDOMBYTESREQUEST')
      call_id = id;
  },
  before(id) {
    process._rawDebug('before')
    if (id === call_id) call_log[0]++;
  },
  after(id) {
    process._rawDebug('after')
    if (id === call_id) call_log[3]++;
  },
}).enable();


process.on('uncaughtException', common.mustCall((err) => {
  process._rawDebug(`uncaught exception!!`, err, (new Error()).stack)
  assert.strictEqual(call_id, async_hooks.executionAsyncId());
  call_log[2]++;
}));


process._rawDebug(`scheduling work`)
require('crypto').randomBytes(1, common.mustCall(() => {
  process._rawDebug(`enter callback`)
  assert.strictEqual(call_id, async_hooks.executionAsyncId());
  call_log[1]++;
  process._rawDebug(`exit callback [throw]`)
  throw new Error('foo');
}));
