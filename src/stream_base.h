#ifndef SRC_STREAM_BASE_H_
#define SRC_STREAM_BASE_H_

#if defined(NODE_WANT_INTERNALS) && NODE_WANT_INTERNALS

#include "env.h"
#include "async_wrap.h"
#include "node.h"
#include "util.h"

#include "v8.h"

namespace node {

struct StreamWriteResult {
  bool async;
  int err;
  size_t bytes;
};

// Forward declarations
class StreamBase;
class StreamResource;

template<typename Base>
class StreamReq {
 public:
  explicit StreamReq(StreamBase* stream) : stream_(stream) {
  }

  inline void Done(int status, const char* error_str = nullptr) {
    Base* req = static_cast<Base*>(this);
    Environment* env = req->env();
    if (error_str != nullptr) {
      req->object()->Set(env->error_string(),
                         OneByteString(env->isolate(), error_str));
    }

    req->OnDone(status);
  }

  inline StreamBase* stream() const { return stream_; }

 private:
  StreamBase* const stream_;
};


// This is the generic interface for objects that control Node.js' C++ streams.
// For example, the default `EmitToJSStreamListener` emits a stream's data
// as Buffers in JS, or `TLSWrap` reads and decrypts data from a stream.
class StreamListener {
 public:
  virtual ~StreamListener();

  // This is called when a stream wants to allocate memory immediately before
  // reading data into the freshly allocated buffer (i.e. it is always followed
  // by a `OnStreamRead()` call).
  // This memory may be statically or dynamically allocated; for example,
  // a protocol parser may want to read data into a static buffer if it knows
  // that all data is going to be fully handled during the next
  // `OnStreamRead()` call.
  // The returned buffer does not need to contain `suggested_size` bytes.
  // The default implementation of this method returns a buffer that has exactly
  // the suggested size and is allocated using malloc().
  virtual uv_buf_t OnStreamAlloc(size_t suggested_size);

  // `OnStreamRead()` is called when data is available on the socket and has
  // been read into the buffer provided by `OnStreamAlloc()`.
  // The `buf` argument is the return value of `uv_buf_t`, or may be a buffer
  // with base nullpptr in case of an error.
  // `nread` is the number of read bytes (which is at most the buffer length),
  // or, if negative, a libuv error code.
  virtual void OnStreamRead(ssize_t nread,
                            const uv_buf_t& buf) = 0;

  // This is called once an *asynchronousÜ Write has finished.
  // `status` may be 0 or, if negative, a libuv error code.
  // For writes that finish synchronously, the `async` flag of the
  // returned StreamWriteResult needs to be inspected.
  virtual void OnStreamAfterWrite(int status) {}
  // This is called once (the writable side of) this stream has been shut down.
  // `status` may be 0 or, if negative, a libuv error code.
  virtual void OnStreamAfterShutdown(int status) {}

  // This is called immediately before the stream is destroyed.
  virtual void OnStreamDestroy() {}

 protected:
  // Pass along a read error to the `StreamListener` instance that was active
  // before this one. For example, a protocol parser does not care about read
  // errors and may instead want to let the original handler
  // (e.g. the JS handler) take care of the situation.
  void PassReadErrorToPreviousListener(ssize_t nread);

  StreamResource* stream_ = nullptr;
  StreamListener* previous_listener_ = nullptr;

  friend class StreamResource;
};


// A default emitter that just pushes data chunks as Buffer instances to
// JS land via the handle’s .ondata method.
class EmitToJSStreamListener : public StreamListener {
 public:
  void OnStreamRead(ssize_t nread, const uv_buf_t& buf) override;
  void OnStreamAfterShutdown(int status) override;
  void OnStreamAfterWrite(int status) override;

 private:
  void CallWithStatus(v8::Local<v8::Name> cbname, int status);
};


// A generic stream, comparable to JS land’s `Duplex` streams.
// A stream is always controlled through one `StreamListener` instance.
class StreamResource {
 public:
  virtual ~StreamResource();

  // For stream implementers: Shut down the writable side of this stream.
  virtual int DoShutdown() = 0;
  // For stream implementers: Write as much data as possible without
  // starting to block the stream; in particular, fully synchronously.
  virtual int DoTryWrite(uv_buf_t** bufs, size_t* count);
  // For stream implementers: Write data to the stream.
  virtual int DoWrite(uv_buf_t* bufs,
                      size_t count,
                      uv_stream_t* send_handle) = 0;

  // Start reading from the underlying resource. This is called by the consumer
  // when more data is desired.
  virtual int ReadStart() = 0;
  // Stop reading from the underlying resource. This is called by the
  // consumer when its buffers are full and no more data can be handled.
  virtual int ReadStop() = 0;

  // Optionally, this may provide an error message to be used for
  // failing writes.
  virtual const char* Error() const;
  // Clear the current error (i.e. that would be returned by Error()).
  virtual void ClearError();

  // Shut down this stream.
  virtual int Shutdown();

  // Writes a list of buffers to the stream, first trying synchronously and,
  // if that is not sufficient, asynchronously.
  // Note: This is currently unimplemented because it is always overridden
  // currently, so not worth the work of doing so, but adding a default
  // implemenation should be trivial if that becomes necessary.
  virtual StreamWriteResult Write(uv_buf_t* bufs,
                                  size_t count,
                                  uv_stream_t* send_handle = nullptr) = 0;

  // Transfer ownership of this tream to `listener`. The previous listener
  // will not receive any more callbacks while the new listener was active.
  void PushStreamListener(StreamListener* listener);
  // Remove a listener, and, if this was the currently active one,
  // transfer ownership back to the previous listener.
  void RemoveStreamListener(StreamListener* listener);

 protected:
  // Call the current listener's OnStreamAlloc() method.
  uv_buf_t EmitAlloc(size_t suggested_size);
  // Call the current listener's OnStreamRead() method and update the
  // stream's read byte counter.
  void EmitRead(ssize_t nread, const uv_buf_t& buf = uv_buf_init(nullptr, 0));
  // Call the current listener's OnStreamAfterWrite() method.
  void EmitAfterWrite(int status);
  // Call the current listener's OnStreamAfterShutdown() method.
  void EmitAfterShutdown(int status);

  StreamListener* listener_ = nullptr;
  uint64_t bytes_read_ = 0;

  friend class StreamListener;
};


class AsyncTrackingStream : public StreamResource {
 public:
  ~AsyncTrackingStream();

  virtual AsyncWrap* GetAsyncWrap() = 0;

  // Shuts down this stream instance, keeping track of async context.
  int Shutdown() override;

  // Writes a list of buffers to the stream, first trying synchronously and,
  // if that is not sufficient, asynchronously while keeping track of
  // async context.
  StreamWriteResult Write(uv_buf_t* bufs,
                          size_t count,
                          uv_stream_t* send_handle = nullptr) override;

  // This is named `stream_env` to avoid name clashes, because a lot of
  // subclasses are also `BaseObject`s.
  Environment* stream_env() const;

 protected:
  explicit AsyncTrackingStream(Environment* env);

  // This is called by the stream implementer after a DoShutdown() call
  // is finished (possibly asynchronously).
  virtual void AfterShutdown(int status);

  // This is called by the stream implementer after a DoWrite() call
  // is finished (possibly asynchronously).
  virtual void AfterWrite(int status);

 private:
  void StartAsyncOperation(AsyncWrap::ProviderType provider);
  template<typename Fn>
  void FinishAsyncOperation(Fn emit_event_cb);

  Environment* env_;
  async_context request_async_context_ = { -1, -1 };
};


class StreamBase : public AsyncTrackingStream {
 public:
  enum Flags {
    kFlagNone = 0x0,
    kFlagHasWritev = 0x1,
    kFlagNoShutdown = 0x2
  };

  template <class Base>
  static inline void AddMethods(Environment* env,
                                v8::Local<v8::FunctionTemplate> target,
                                int flags = kFlagNone);

  virtual bool IsAlive() = 0;
  virtual bool IsClosing() = 0;
  virtual bool IsIPCPipe();
  virtual int GetFD();

  void CallJSOnreadMethod(ssize_t nread, v8::Local<v8::Object> buf);

  // These are overridden methods from AsyncTrackingStream that do a
  // little more bookkeeping; in particular, they store errors on the stream
  // object and make sure the extra_storage_ field for write buffers is reset.
  StreamWriteResult Write(uv_buf_t* bufs,
                          size_t count,
                          uv_stream_t* send_handle = nullptr) override;
  void AfterWrite(int status) override;
  void AfterShutdown(int status) override;

  v8::Local<v8::Object> GetObject();

 protected:
  explicit StreamBase(Environment* env);

  // JS Methods
  int ReadStartJS(const v8::FunctionCallbackInfo<v8::Value>& args);
  int ReadStopJS(const v8::FunctionCallbackInfo<v8::Value>& args);
  int ShutdownJS(const v8::FunctionCallbackInfo<v8::Value>& args);
  int Writev(const v8::FunctionCallbackInfo<v8::Value>& args);
  int WriteBuffer(const v8::FunctionCallbackInfo<v8::Value>& args);
  template <enum encoding enc>
  int WriteString(const v8::FunctionCallbackInfo<v8::Value>& args);

  template <class Base>
  static void GetFD(const v8::FunctionCallbackInfo<v8::Value>& args);

  template <class Base>
  static void GetExternal(const v8::FunctionCallbackInfo<v8::Value>& args);

  template <class Base>
  static void GetBytesRead(const v8::FunctionCallbackInfo<v8::Value>& args);

  template <class Base,
            int (StreamBase::*Method)(
      const v8::FunctionCallbackInfo<v8::Value>& args)>
  static void JSMethod(const v8::FunctionCallbackInfo<v8::Value>& args);

 private:
  void SetErrorOnObject();
  bool AllocateWriteStorage(size_t storage_size);
  void SetWriteMetadata(bool async, int err, size_t bytes);

  EmitToJSStreamListener default_listener_;
  char* extra_storage_ = nullptr;
};

}  // namespace node

#endif  // defined(NODE_WANT_INTERNALS) && NODE_WANT_INTERNALS

#endif  // SRC_STREAM_BASE_H_
