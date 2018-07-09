// Flags: --expose-internals
'use strict';
const common = require('../common');
const assert = require('assert');
const { createJSHeapDump, buildEmbedderGraph } = require('internal/test/heap');

// validateSnapshotNodes('DNSCHANNEL', { expected: 0 });
const dns = require('dns');
validateSnapshotNodes('DNSCHANNEL', { expected: 1 });

function validateSnapshotNodes(name, { expected }) {
  const snapshot = createJSHeapDump().filter((node) => node.name === name && node.type !== 'string');
//  const graph = buildEmbedderGraph().filter((node) => node.name === name);
  console.dir(snapshot, {depth:6});
//  console.dir(graph, {depth:4})
}
