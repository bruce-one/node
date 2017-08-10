// Flags: --expose-http2
'use strict';

const common = require('../common');
if (!common.hasCrypto)
  common.skip('missing crypto');
const assert = require('assert');
const http2 = require('http2');

const server = http2.createServer();

const src = {
  'regular-header': 'foo',
  [http2.noStore]: {
    'unindexed-header': 'A'.repeat(1000)
  }
};

function checkHeaders(headers) {
  assert.strictEqual(headers['regular-header'], 'foo');
  assert.strictEqual(headers['unindexed-header'], 'A'.repeat(1000));
}

server.on('stream', common.mustCall((stream, headers) => {
  checkHeaders(headers);
  stream.respond(src);
  stream.end();
}));

server.listen(0, common.mustCall(() => {
  const client = http2.connect(`http://localhost:${server.address().port}`);
  const req = client.request(src);
  req.on('response', common.mustCall(checkHeaders));
  req.on('streamClosed', common.mustCall(() => {
    server.close();
    client.destroy();
  }));
}));
