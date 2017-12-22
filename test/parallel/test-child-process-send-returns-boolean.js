'use strict';
const common = require('../common');

// libuv does not support uv_try_write() for pipes on Windows.
const trueExceptOnWindows = common.isWindows ? false : true;

// subprocess.send() will return false if the channel has closed or when the
// backlog of unsent messages exceeds a threshold that makes it unwise to send
// more. Otherwise, the method returns true.

const assert = require('assert');
const net = require('net');
const { fork, spawn } = require('child_process');
const fixtures = require('../common/fixtures');

// Just a script that stays alive (does not listen to `process.on('message')`).
const subScript = fixtures.path('child-process-persistent.js');

{
  // Test `send` return value on `fork` that opens and IPC by default.
  const n = fork(subScript);
  // `subprocess.send` should always return `true` for the first send.
  const rv = n.send({ h: 'w' }, (err) => { if (err) assert.fail(err); });
  assert.strictEqual(rv, true);
  n.kill();
}

{
  // Test `send` return value on `spawn` and saturate backlog with handles.
  // Call `spawn` with options that open an IPC channel.
  const spawnOptions = { stdio: ['pipe', 'pipe', 'pipe', 'ipc'] };
  const s = spawn(process.execPath, [subScript], spawnOptions);

  const server = net.createServer(common.mustNotCall()).listen(0, () => {
    const handle = server._handle;

    // Sending a handle will always trigger an asynchronous write.
    const rv1 = s.send('one', handle, (err) => assert.ifError(err));
    assert.strictEqual(rv1, false);
    // Since the first `send` included a handle (should be unacknowledged),
    // subqeuent synchronous writes will always be turned async.
    const rv2 = s.send('two', common.mustCall((err) => {
      if (err) assert.fail(err);
      // The backlog should now be clear to backoff.
      const rv3 = s.send('three', (err) => assert.ifError(err));
      assert.strictEqual(rv3, trueExceptOnWindows);
      const rv4 = s.send('four', common.mustCall((err) => {
        if (err) assert.fail(err);
        // `send` queue should have been drained.
        // Handle writes are still asynchronous.
        const rv5 = s.send('5', handle, (err) => assert.ifError(err));
        assert.strictEqual(rv5, false);

        // End test and cleanup.
        s.kill();
        handle.close();
        server.close();
      }));
      assert.strictEqual(rv4, trueExceptOnWindows);
    }));
    assert.strictEqual(rv2, false);
  });
}
