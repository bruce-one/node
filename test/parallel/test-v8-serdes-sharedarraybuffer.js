/*global SharedArrayBuffer*/
'use strict';
// Flags: --harmony-sharedarraybuffer

const common = require('../common');
const assert = require('assert');
const serdes = require('serdes');

{
  const sab = new SharedArrayBuffer(64);
  const uint8array = new Uint8Array(sab);
  const ID = 42;

  const ser = new serdes.Serializer();
  ser._getSharedArrayBufferId = common.mustCall(() => ID);
  ser.writeHeader();

  ser.writeValue(uint8array);

  const des = new serdes.Deserializer(ser.releaseBuffer());
  des.readHeader();
  des.transferArrayBuffer(ID, sab);

  const value = des.readValue();
  assert.strictEqual(value.buffer, sab);
  assert.notStrictEqual(value, uint8array);
}
