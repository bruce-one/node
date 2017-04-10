'use strict';

const Buffer = require('buffer').Buffer;

const serdesBinding = process.binding('serdes');
const bufferBinding = process.binding('buffer');

const { objectToString } = require('internal/util');
const { FastBuffer } = require('internal/buffer');

const Serializer = exports.Serializer = serdesBinding.Serializer;
const Deserializer = exports.Deserializer = serdesBinding.Deserializer;

/* JS methods for the base objects */
Serializer.prototype._getDataCloneError = Error;

Deserializer.prototype.readRawBytes = function(length) {
  const offset = this._readRawBytes(length);
  // `this.buffer` can be a Buffer or a plain Uint8Array, so just calling
  // `.slice()` doesn't work.
  return new FastBuffer(this.buffer.buffer,
                        this.buffer.byteOffset + offset,
                        length);
};

/* Keep track of how to handle different ArrayBufferViews.
 * The default Serializer for Node does not use the V8 methods for serializing
 * those objects because Node's `Buffer` objects use pooled allocation in many
 * cases, and their underlying `ArrayBuffer`s would show up in the
 * serialization. Because a) those may contain sensitive data and the user
 * may not be aware of that and b) they are often much larger than the `Buffer`
 * itself, custom serialization is applied. */
const arrayBufferViewTypes = [Int8Array, Uint8Array, Uint8ClampedArray,
                              Int16Array, Uint16Array, Int32Array, Uint32Array,
                              Float32Array, Float64Array, DataView];

const arrayBufferViewTypeToIndex = new Map();

{
  const dummy = new ArrayBuffer();
  for (const [i, ctor] of arrayBufferViewTypes.entries()) {
    const tag = objectToString(new ctor(dummy));
    arrayBufferViewTypeToIndex.set(tag, i);
  }
}

const bufferConstructorIndex = arrayBufferViewTypes.push(Buffer) - 1;

class DefaultSerializer extends Serializer {
  constructor() {
    super();

    this._setTreatArrayBufferViewsAsHostObjects(true);
  }

  _writeHostObject(abView) {
    let i = 0;
    if (abView.constructor === Buffer) {
      i = bufferConstructorIndex;
    } else {
      const tag = objectToString(abView);
      i = arrayBufferViewTypeToIndex.get(tag);

      if (i === undefined) {
        throw this._getDataCloneError(`Unknown host object type: ${tag}`);
      }
    }
    this.writeUint32(i);
    this.writeUint32(abView.byteLength);
    this.writeRawBytes(new Uint8Array(abView.buffer,
                                      abView.byteOffset,
                                      abView.byteLength));
  }
}

exports.DefaultSerializer = DefaultSerializer;

class DefaultDeserializer extends Deserializer {
  constructor(buffer) {
    super(buffer);
  }

  _readHostObject() {
    const typeIndex = this.readUint32();
    const ctor = arrayBufferViewTypes[typeIndex];
    const byteLength = this.readUint32();
    const byteOffset = this._readRawBytes(byteLength);
    const BYTES_PER_ELEMENT = ctor.BYTES_PER_ELEMENT || 1;

    const offset = this.buffer.byteOffset + byteOffset;
    if (offset % BYTES_PER_ELEMENT === 0) {
      return new ctor(this.buffer.buffer,
                      offset,
                      byteLength / BYTES_PER_ELEMENT);
    } else {
      // Copy to an aligned buffer first.
      const copy = Buffer.allocUnsafe(byteLength);
      bufferBinding.copy(this.buffer, copy, 0,
                         byteOffset, byteOffset + byteLength);
      return new ctor(copy.buffer,
                      copy.byteOffset,
                      byteLength / BYTES_PER_ELEMENT);
    }
  }
}

exports.DefaultDeserializer = DefaultDeserializer;

exports.serialize = function serialize(value) {
  const ser = new DefaultSerializer();
  ser.writeHeader();
  ser.writeValue(value);
  return ser.releaseBuffer();
};

exports.deserialize = function deserialize(buffer) {
  const der = new DefaultDeserializer(buffer);
  der.readHeader();
  return der.readValue();
};
