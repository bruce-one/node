# Serialization API

> Stability: 1 - Experimental

The serialization API provides means of serializing JavaScript values in a way
that is compatible with the [HTML structured clone algorithm][].
The format is backward-compatible (i.e. safe to store to disk).

*Note*: This API is under development, and changes (including incompatible
changes to the API or wire format) may occur until this warning is removed.

## serdes.serialize(value)
<!--
added: REPLACEME
-->

* Returns: {Buffer}

Uses a [`DefaultSerializer`][] to serialize `value` into a buffer.

## serdes.deserialize(buffer)
<!--
added: REPLACEME
-->

* `buffer` {Buffer|Uint8Array} A buffer returned by [`serialize()`][].

Uses a [`DefaultDeserializer`][] with default options to read a JS value
from a buffer.

## class: serdes.Serializer
<!--
added: REPLACEME
-->

### new Serializer()
Creates a new `Serializer` object.

### serializer.writeHeader()

Writes out a header, which includes the serialization format version.

### serializer.writeValue(value)

Serializes a JavaScript value and adds the serialized representation to the
internal buffer.

This throws an error if `value` cannot be serialized.

### serializer.releaseBuffer()

Returns the stored internal buffer. This serializer should not be used once
the buffer is released. Calling this method results in undefined behavior
if a previous write has failed.

### serializer.transferArrayBuffer(id, arrayBuffer)

* `id` {integer} A 32-bit unsigned integer.
* `arrayBuffer` {ArrayBuffer} An `ArrayBuffer` instance.

Marks an `ArrayBuffer` as havings its contents transferred out of band.
Pass the corresponding `ArrayBuffer` in the deserializing context to
[`deserializer.transferArrayBuffer()`][].

### serializer.writeUint32(value)

* `value` {integer}

Write a raw 32-bit unsigned integer.
For use inside of a custom [`serializer._writeHostObject()`][].

### serializer.writeUint64(hi, lo)

* `hi` {integer}
* `lo` {integer}

Write a raw 64-bit unsigned integer, split into high and low 32-bit parts.
For use inside of a custom [`serializer._writeHostObject()`][].

### serializer.writeDouble(value)

* `value` {number}

Write a JS `number` value.
For use inside of a custom [`serializer._writeHostObject()`][].

### serializer.writeRawBytes(buffer)

* `buffer` {Buffer|Uint8Array}

Write raw bytes into the serializer’s internal buffer. The deserializer
will require a way to compute the length of the buffer.
For use inside of a custom [`serializer._writeHostObject()`][].

### serializer.\_writeHostObject(object)

* `object` {Object}

This method is called to write some kind of host object, i.e. an object created
by native C++ bindings. If it is not possible to serialize `object`, a suitable
exception should be thrown.

This method is not present on the `Serializer` class itself but can be provided
by subclasses.

### serializer.\_getDataCloneError(message)

* `message` {string}

This method is called to generate error objects that will be thrown when an
object can not be cloned.

This method defaults to the [`Error`][] constructor and can be be overridden on
subclasses.

### serializer.\_getSharedArrayBufferId(sharedArrayBuffer)

* `sharedArrayBuffer` {SharedArrayBuffer}

This method is called when the serializer is going to serialize a
`SharedArrayBuffer` object. It must return an unsigned 32-bit integer ID for
the object, using the same ID if this `SharedArrayBuffer` has already been
serialized. When deserializing, this ID will be passed to
[`deserializer.transferArrayBuffer()`][].

If the object cannot be serialized, an exception should be thrown.

This method is not present on the `Serializer` class itself but can be provided
by subclasses.

### serializer.\_setTreatArrayBufferViewsAsHostObjects(flag)

* `flag` {boolean}

Indicate whether to treat `TypedArray` and `DataView` objects as
host objects, i.e. pass them to [`serializer._writeHostObject`][].

The default is not to treat those objects as host objects.

## class: serdes.Deserializer
<!--
added: REPLACEME
-->

### new Deserializer(buffer)

* `buffer` {Buffer|Uint8Array} A buffer returned by [`serializer.releaseBuffer()`][].

Creates a new `Deserializer` object.

### deserializer.readHeader()

Reads and validates a header (including the format version).
May, for example, reject an invalid or unsupported wire format. In that case,
an `Error` is thrown.

### deserializer.readValue()

Deserializes a JavaScript value from the buffer and returns it.

### deserializer.transferArrayBuffer(id, arrayBuffer)

* `id` {integer} A 32-bit unsigned integer.
* `arrayBuffer` {ArrayBuffer|SharedArrayBuffer} An `ArrayBuffer` instance.

Marks an `ArrayBuffer` as havings its contents transferred out of band.
Pass the corresponding `ArrayBuffer` in the serializing context to
[`serializer.transferArrayBuffer()`][] (or return the `id` from
[`serializer._getSharedArrayBufferId()`][] in the case of `SharedArrayBuffer`s).

### deserializer.getWireFormatVersion()

* Returns: {integer}

Reads the underlying wire format version. Likely mostly to be useful to
legacy code reading old wire format versions. May not be called before
`.readHeader()`.

### deserializer.readUint32()

* Returns: {integer}

Read a raw 32-bit unsigned integer and return it.
For use inside of a custom [`deserializer._readHostObject()`][].

### deserializer.readUint64()

* Returns: {Array}

Read a raw 64-bit unsigned integer and return it as an array `[hi, lo]`
with two 32-bit unsigned integer entries.
For use inside of a custom [`deserializer._readHostObject()`][].

### deserializer.readDouble()

* Returns: {number}

Read a JS `number` value.
For use inside of a custom [`deserializer._readHostObject()`][].

### deserializer.readRawBytes(length)

* Returns: {Buffer}

Read raw bytes from the deserializer’s internal buffer. The `length` parameter
must correspond to the length of the buffer that was passed to
[`serializer.writeRawBytes()`][].
For use inside of a custom [`deserializer._readHostObject()`][].

### deserializer.\_readHostObject()

This method is called to read some kind of host object, i.e. an object that is
created by native C++ bindings. If it is not possible to deserialize the data,
a suitable exception should be thrown.

This method is not present on the `Deserializer` class itself but can be
provided by subclasses.

## class: serdes.DefaultSerializer
<!--
added: REPLACEME
-->

A subclass of [`Serializer`][] that serializes `TypedArray`
(in particular [`Buffer`][]) and `DataView` objects as host objects, and only
stores the part of their underlying `ArrayBuffer`s that they are referring to.

## class: serdes.DefaultDeserializer
<!--
added: REPLACEME
-->

A subclass of [`Deserializer`][] corresponding to the format written by
[`DefaultSerializer`][].

[`Buffer`]: buffer.html
[`Error`]: errors.html#errors_class_error
[`deserializer.transferArrayBuffer()`]: #serdes_deserializer_transferarraybuffer_id_arraybuffer
[`deserializer._readHostObject()`]: #serdes_deserializer_readhostobject
[`serializer.transferArrayBuffer()`]: #serdes_serializer_transferarraybuffer_id_arraybuffer
[`serializer.releaseBuffer()`]: #serdes_serializer_releasebuffer
[`serializer.writeRawBytes()`]: #serdes_serializer_writerawbytes_buffer
[`serializer._writeHostObject()`]: #serdes_serializer_writehostobject_object
[`serializer._getSharedArrayBufferId()`]: #serdes_serializer_getsharedarraybufferid_sharedarraybuffer
[`Serializer`]: #serdes_class_serdes_serializer
[`Deserializer`]: #serdes_class_serdes_deserializer
[`DefaultSerializer`]: #serdes_class_serdes_defaultserializer
[`DefaultDeserializer`]: #serdes_class_serdes_defaultdeserializer
[`serialize()`]: #serdes_serdes_serialize_value
[HTML structured clone algorithm]: https://developer.mozilla.org/en-US/docs/Web/API/Web_Workers_API/Structured_clone_algorithm
