'use strict';
// Flags: --expose-internals
const common = require('../common');
const { Duplex, Readable } = require('stream');
const JSStreamWrap = require('internal/wrap_js_stream');
const assert = require('assert');
const { promisify, inspect } = require('util');
const tick = promisify(setImmediate);

common.crashOnUnhandledRejection();

const tests = [
  [ '666f6f', '80', { encoding: 'utf8' }, '80808080' ],
  [ { encoding: 'utf16le' }, '41', { encoding: 'utf16le' } ],
  [ { encoding: 'utf16le' }, '41' ],
  [ { encoding: 'utf16le' }, '4100' ],
  [ { encoding: 'utf16le' }, '41' ],
  [ { encoding: 'utf16le' }, '41', '41' ],
  [ { encoding: 'utf16le' }, '4100', '41', '41' ],
  [ { encoding: 'utf16le' }, '3c', 'd8', '89', 'df' ],
  [ { encoding: 'utf16le' }, '3cd889', 'df' ],
  [ { encoding: 'utf16le' }, '3cd8', '89df' ],
  [ { encoding: 'utf16le' }, 'd8c3', 'df89' ],
  [ { encoding: 'utf16le' }, '3c', 'd889', 'df' ],
  [ { encoding: 'utf16le' }, '3c', 'd8' ],
  [ { encoding: 'utf16le' }, '3c', 'd8', '89' ],
  [ { encoding: 'utf16le' }, '3cd8aaaa89df' ],
  [ { encoding: 'utf16le' }, 'aaaa3cd8', 'aaaa89df' ],
  [ { encoding: 'utf16le' }, 'aaaa3cd8', '89dfaaaa' ],
  [ { encoding: 'utf16le' }, 'd8', '3cdf', '89' ],
  [ { encoding: 'utf16le' }, 'd8', '3c', 'df', '89' ],
  [ { encoding: 'utf16le' }, 'd83cdf', '89' ],
  [ { encoding: 'utf16le' }, 'd83cdf' ],
  [ { encoding: 'utf8' }, '3c', 'd889', 'df' ],
  [ { encoding: 'utf8' }, 'f0', '9f', '8e', '89' ],
  [ { encoding: 'utf8' }, 'f09f', '8e89' ],
  [ { encoding: 'utf8' }, 'f0', '9f8e89' ],
  [ { encoding: 'utf8' }, 'f0', '9f8e', '89' ],
  [ { encoding: 'utf8' }, 'f0', '9f8e' ],
  [ 'f0', { encoding: 'utf8' }, 'f0', '9f8e' ],
  [ 'f0', { encoding: 'utf8' }, '80', '9f8e' ],
  [ 'f0', { encoding: 'utf8' }, '80', '8080' ],
  [ 'f0', { encoding: 'utf8' }, '8080808080' ],
  [ 'f0', { encoding: 'utf8' }, 'c0c0c0c080' ],
  [ 'f0', { encoding: 'utf8' }, 'c0c0c0c0c3' ],
  [ 'f0', { encoding: 'utf8' }, 'c0c0c0c3b6' ],
  [ { encoding: 'utf8' }, 'c0c0c0c0c0' ],
  [ { encoding: 'utf8' }, '4a4a4a4a4a' ],
  [ { encoding: 'utf8' }, '4a4a4a4a4ac0c0c0c0c0', 'utf16le', '41', '00' ],
  [ { encoding: 'utf8' }, 'c3', { encoding: 'utf8' }, 'b6' ],
  [ { encoding: 'utf8' }, 'c3', { encoding: 'utf8' }, 'c3' ],
  [ { encoding: 'hex' }, 'c3' ],
  [ { encoding: 'ascii' }, 'c3' ],
  [ { encoding: 'ascii' }, '43' ],
  [ { encoding: 'latin1' }, 'c3' ],
  [ { encoding: 'latin1' }, '43' ],
  [ { encoding: 'base64' }, 'c3' ],
  [ { encoding: 'base64' }, 'c3aa' ],
  [ { encoding: 'base64' }, 'c3aaaa' ],
  [ { encoding: 'base64' }, '0000000' ],
  [ { encoding: 'base64' }, '000000001' ],
  [ { encoding: 'base64' }, '0000001' ],
  [ { encoding: 'base64' }, '0001' ],
  [ { encoding: 'base64' }, '01' ],
];

(async function() {
  for (const contents of tests) {
    const readable = new Readable({ read() {} });
    const stream = new Duplex({ read() {}, write() {} });
    const socket = new JSStreamWrap(stream);
    for (const chunk of contents.concat([null])) {
      if (chunk && chunk.encoding) {
        socket.setEncoding(chunk.encoding);
        readable.setEncoding(chunk.encoding);
        continue;
      }

      const buf = chunk === null ? null : Buffer.from(chunk, 'hex');

      stream.push(buf);
      readable.push(buf);
      await tick();

      while (true) {
        const actual = socket.read();
        const expected = readable.read();
        assert.deepStrictEqual(
          actual, expected, 'Mismatched output:\n' +
                            `Actual: ${inspect(actual)}\n` +
                            `Expected: ${inspect(expected)}\n` +
                            `Input: ${inspect(buf)}\n` +
                            `Full test: ${inspect(contents)}\n` +
                            `Encoding: ${socket._readableState.encoding}\n`);
        if (actual === null)
          break;
      }
    }
  }
})();
