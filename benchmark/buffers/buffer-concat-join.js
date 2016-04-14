'use strict';
const common = require('../common.js');

const bench = common.createBenchmark(main, {
  type: ['concat', 'join'],
  n: [30000],
  nBuffers: [16, 32],
  bufferSize: [16, 128, 1024]
});

function main(conf) {
  const n = +conf.n;
  const bufList = [...Array(+conf.nBuffers).keys()]
    .map(() => Buffer.allocUnsafe(1024));

  bench.start();
  for (var i = 0; i < n; i++) {
    Buffer[conf.type](bufList);
  }
  bench.end(n);
}
