'use strict';
const common = require('../common');
const assert = require('assert');

const StreamWrap = require('_stream_wrap');
const Duplex = require('stream').Duplex;

function testShutdown(callback) {
  const stream = new Duplex({
    read: function() {
    },
    write: function() {
    }
  });

  const wrap = new StreamWrap(stream);

  const handle = wrap._handle;

  // Close the handle to simulate
  handle.shutdown();
  wrap.destroy();

  handle.onaftershutdown = function(code) {
    assert(code < 0);
    callback();
  };
}

testShutdown(common.mustCall());
