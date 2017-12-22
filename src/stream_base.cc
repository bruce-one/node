#include "stream_base-inl.h"
#include "stream_wrap.h"

#include "async_wrap-inl.h"
#include "node_internals.h"
#include "node_buffer.h"
#include "env-inl.h"
#include "js_stream.h"
#include "string_bytes.h"
#include "util-inl.h"
#include "v8.h"

#include <limits.h>  // INT_MAX

namespace node {

using v8::Array;
using v8::Context;
using v8::Function;
using v8::FunctionCallbackInfo;
using v8::HandleScope;
using v8::Integer;
using v8::Local;
using v8::Name;
using v8::Object;
using v8::String;
using v8::Value;

template int StreamBase::WriteString<ASCII>(
    const FunctionCallbackInfo<Value>& args);
template int StreamBase::WriteString<UTF8>(
    const FunctionCallbackInfo<Value>& args);
template int StreamBase::WriteString<UCS2>(
    const FunctionCallbackInfo<Value>& args);
template int StreamBase::WriteString<LATIN1>(
    const FunctionCallbackInfo<Value>& args);


int StreamBase::ReadStartJS(const FunctionCallbackInfo<Value>& args) {
  return ReadStart();
}


int StreamBase::ReadStopJS(const FunctionCallbackInfo<Value>& args) {
  return ReadStop();
}


int StreamBase::ShutdownJS(const FunctionCallbackInfo<Value>& args) {
  // TODO(addaleax): Ideally, this and all the other JS-accessible methods
  // should somehow verify that the current listener is either the default
  // one or the one installed by LibuvStreamWrap.
  // (Alternatively, the LibuvStreamWrap special listener might be merged
  // into the default one?)
  return Shutdown();
}


int AsyncTrackingStream::Shutdown() {
  StartAsyncOperation(AsyncWrap::PROVIDER_SHUTDOWNWRAP);
  int err = DoShutdown();
  if (err) {
    AsyncWrap::EmitDestroy(env_, request_async_context_.async_id);
    request_async_context_ = { -1, -1 };
  }
  return err;
}


template<typename Fn>
void AsyncTrackingStream::FinishAsyncOperation(Fn emit_event_cb) {
  async_context ctx = request_async_context_;
  CHECK_NE(ctx.async_id, -1);
  request_async_context_ = { -1, -1 };

  HandleScope handle_scope(env_->isolate());
  Context::Scope context_scope(env_->context());

  {
    InternalCallbackScope cb_scope(env_,
                                   Local<Object>(),
                                   ctx,
                                   InternalCallbackScope::kAllowEmptyResource);

    emit_event_cb();
  }

  AsyncWrap::EmitDestroy(env_, ctx.async_id);
}

void AsyncTrackingStream::AfterShutdown(int status) {
  FinishAsyncOperation([&]() {
    EmitAfterShutdown(status);
  });
}

void AsyncTrackingStream::AfterWrite(int status) {
  FinishAsyncOperation([&]() {
    EmitAfterWrite(status);
  });
}


void AsyncTrackingStream::StartAsyncOperation(
    AsyncWrap::ProviderType provider) {
  CHECK_EQ(request_async_context_.async_id, -1);

  Local<Object> async_resource;
  AsyncWrap* wrap = GetAsyncWrap();

  double id = env_->new_async_id();
  request_async_context_ = { id, wrap->get_trigger_async_id() };

  if (env_->async_hooks()->fields()[AsyncHooks::kInit] == 0)
    return;
  async_resource = Object::New(env_->isolate());

  Local<String> resource_type =
      env_->async_hooks()->provider_string(provider);
  AsyncWrap::EmitAsyncInit(env_,
                           async_resource,
                           resource_type,
                           id,
                           wrap->get_async_id());
}


bool StreamBase::AllocateWriteStorage(size_t storage_size) {
  CHECK_EQ(extra_storage_, nullptr);
  if (storage_size == 0)
    return true;
  extra_storage_ = node::UncheckedMalloc(storage_size);
  return extra_storage_ != nullptr;
}


int StreamBase::Writev(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);

  CHECK(args[0]->IsArray());

  Local<Array> chunks = args[0].As<Array>();
  bool all_buffers = args[1]->IsTrue();

  size_t count;
  if (all_buffers)
    count = chunks->Length();
  else
    count = chunks->Length() >> 1;

  MaybeStackBuffer<uv_buf_t, 16> bufs(count);
  uv_buf_t* buf_list = *bufs;

  size_t storage_size = 0;
  uint32_t bytes = 0;
  size_t offset;

  constexpr size_t kAlignSize = 16;

  if (!all_buffers) {
    // Determine storage size first
    for (size_t i = 0; i < count; i++) {
      storage_size = ROUND_UP(storage_size, kAlignSize);

      Local<Value> chunk = chunks->Get(i * 2);

      if (Buffer::HasInstance(chunk))
        continue;
        // Buffer chunk, no additional storage required

      // String chunk
      Local<String> string = chunk->ToString(env->context()).ToLocalChecked();
      enum encoding encoding = ParseEncoding(env->isolate(),
                                             chunks->Get(i * 2 + 1));
      size_t chunk_size;
      if (encoding == UTF8 && string->Length() > 65535)
        chunk_size = StringBytes::Size(env->isolate(), string, encoding);
      else
        chunk_size = StringBytes::StorageSize(env->isolate(), string, encoding);

      storage_size += chunk_size;
    }

    if (storage_size > INT_MAX)
      return UV_ENOBUFS;
  } else {
    for (size_t i = 0; i < count; i++) {
      Local<Value> chunk = chunks->Get(i);
      bufs[i].base = Buffer::Data(chunk);
      bufs[i].len = Buffer::Length(chunk);
      bytes += bufs[i].len;
    }
  }

  if (!AllocateWriteStorage(storage_size)) {
    return UV_ENOMEM;
  }

  offset = 0;
  if (!all_buffers) {
    for (size_t i = 0; i < count; i++) {
      Local<Value> chunk = chunks->Get(i * 2);

      // Write buffer
      if (Buffer::HasInstance(chunk)) {
        bufs[i].base = Buffer::Data(chunk);
        bufs[i].len = Buffer::Length(chunk);
        bytes += bufs[i].len;
        continue;
      }

      // Write string
      offset = ROUND_UP(offset, kAlignSize);
      CHECK_LE(offset, storage_size);
      char* str_storage = extra_storage_ + offset;
      size_t str_size = storage_size - offset;

      Local<String> string = chunk->ToString(env->context()).ToLocalChecked();
      enum encoding encoding = ParseEncoding(env->isolate(),
                                             chunks->Get(i * 2 + 1));
      str_size = StringBytes::Write(env->isolate(),
                                    str_storage,
                                    str_size,
                                    string,
                                    encoding);
      bufs[i].base = str_storage;
      bufs[i].len = str_size;
      offset += str_size;
      bytes += str_size;
    }
  }

  return Write(buf_list, count).err;
}


int StreamBase::WriteBuffer(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);

  if (!args[0]->IsUint8Array()) {
    env->ThrowTypeError("Second argument must be a buffer");
    return 0;
  }

  const char* data = Buffer::Data(args[0]);
  size_t length = Buffer::Length(args[0]);

  uv_buf_t buf;
  buf.base = const_cast<char*>(data);
  buf.len = length;

  return Write(&buf, 1).err;
}


template <enum encoding enc>
int StreamBase::WriteString(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  CHECK(args[0]->IsString());

  Local<String> string = args[0].As<String>();
  Local<Object> send_handle_obj;
  if (args[1]->IsObject())
    send_handle_obj = args[1].As<Object>();

  int err;

  // Compute the size of the storage that the string will be flattened into.
  // For UTF8 strings that are very long, go ahead and take the hit for
  // computing their actual size, rather than tripling the storage.
  size_t storage_size;
  if (enc == UTF8 && string->Length() > 65535)
    storage_size = StringBytes::Size(env->isolate(), string, enc);
  else
    storage_size = StringBytes::StorageSize(env->isolate(), string, enc);

  if (storage_size > INT_MAX)
    return UV_ENOBUFS;

  // Try writing immediately if write size isn't too big
  char stack_storage[16384];  // 16kb
  size_t data_size;
  uv_buf_t buf;

  bool try_write = storage_size <= sizeof(stack_storage) &&
                   (!IsIPCPipe() || send_handle_obj.IsEmpty());
  if (try_write) {
    data_size = StringBytes::Write(env->isolate(),
                                   stack_storage,
                                   storage_size,
                                   string,
                                   enc);
    buf = uv_buf_init(stack_storage, data_size);

    uv_buf_t* bufs = &buf;
    size_t count = 1;
    err = DoTryWrite(&bufs, &count);

    if (err != 0 || count == 0) {
      StreamWriteResult res;
      res.async = false;
      res.err = err;
      res.bytes = data_size;
      env->fill_write_info_buffer(res);
      return err;
    }

    // Partial write
    CHECK_EQ(count, 1);
  }

  if (!AllocateWriteStorage(storage_size))
    return UV_ENOMEM;

  if (try_write) {
    // Copy partial data
    memcpy(extra_storage_, buf.base, buf.len);
    data_size = buf.len;
  } else {
    // Write it
    data_size = StringBytes::Write(env->isolate(),
                                   extra_storage_,
                                   storage_size,
                                   string,
                                   enc);
  }

  buf = uv_buf_init(extra_storage_, data_size);

  uv_stream_t* send_stream = nullptr;

  if (!send_handle_obj.IsEmpty() && IsIPCPipe()) {
    uv_handle_t* send_handle = nullptr;

    HandleWrap* wrap;
    ASSIGN_OR_RETURN_UNWRAP(&wrap, send_handle_obj, UV_EINVAL);
    send_handle = wrap->GetHandle();
    send_stream = reinterpret_cast<uv_stream_t*>(send_handle);
  }

  return Write(&buf, 1, send_stream).err;
}


StreamWriteResult AsyncTrackingStream::Write(uv_buf_t* bufs,
                                             size_t count,
                                             uv_stream_t* send_handle) {
  CHECK_EQ(request_async_context_.async_id, -1);

  int err = 0;
  size_t bytes = 0;

  auto finish_write = [&](bool async) -> StreamWriteResult {
    StreamWriteResult ret = { async, err, bytes };

    // Write information to a buffer that is used for carrying information
    // about the write status back to JS.
    // The actual writes into the buffer happen just before returning from the
    // function, since it's possible that DoWrite() itself also calls some other
    // stream's Write() method.
    env_->fill_write_info_buffer(ret);
    return ret;
  };

  for (size_t i = 0; i < count; ++i)
    bytes += bufs[i].len;

  if (send_handle == nullptr) {
    err = DoTryWrite(&bufs, &count);
    if (count == 0 || err != 0) {
      return finish_write(false);
    }
  }

  StartAsyncOperation(AsyncWrap::PROVIDER_WRITEWRAP);

  if (err != 0) {
    AsyncWrap::EmitDestroy(env_, request_async_context_.async_id);
    request_async_context_ = { -1, -1 };
  }

  return finish_write(err == 0);
}


void StreamBase::AfterShutdown(int status) {
  SetErrorOnObject();
  AsyncTrackingStream::AfterShutdown(status);
}


StreamWriteResult StreamBase::Write(uv_buf_t* bufs,
                                    size_t count,
                                    uv_stream_t* send_handle) {
  StreamWriteResult res = AsyncTrackingStream::Write(bufs, count, send_handle);
  if (!res.async) {
    free(extra_storage_);
    extra_storage_ = nullptr;
  }
  SetErrorOnObject();
  return res;
}

void StreamBase::AfterWrite(int status) {
  free(extra_storage_);
  extra_storage_ = nullptr;
  SetErrorOnObject();
  AsyncTrackingStream::AfterWrite(status);
}


void StreamBase::SetErrorOnObject() {
  Environment* env = stream_env();
  const char* msg = Error();
  if (msg != nullptr) {
    GetObject()->Set(env->context(),
                     env->error_string(),
                     OneByteString(env->isolate(), msg)).FromJust();
    ClearError();
  }
}


void StreamBase::CallJSOnreadMethod(ssize_t nread, Local<Object> buf) {
  Environment* env = stream_env();

  Local<Value> argv[] = {
    Integer::New(env->isolate(), nread),
    buf
  };

  if (argv[1].IsEmpty())
    argv[1] = Undefined(env->isolate());

  AsyncWrap* wrap = GetAsyncWrap();
  CHECK_NE(wrap, nullptr);
  wrap->MakeCallback(env->onread_string(), arraysize(argv), argv);
}


bool StreamBase::IsIPCPipe() {
  return false;
}


int StreamBase::GetFD() {
  return -1;
}


int StreamResource::DoTryWrite(uv_buf_t** bufs, size_t* count) {
  // No TryWrite by default
  return 0;
}


const char* StreamResource::Error() const {
  return nullptr;
}


void StreamResource::ClearError() {
  // No-op
}


uv_buf_t StreamListener::OnStreamAlloc(size_t suggested_size) {
  return uv_buf_init(Malloc(suggested_size), suggested_size);
}


void EmitToJSStreamListener::OnStreamRead(ssize_t nread, const uv_buf_t& buf) {
  CHECK_NE(stream_, nullptr);
  StreamBase* stream = static_cast<StreamBase*>(stream_);
  Environment* env = stream->stream_env();
  HandleScope handle_scope(env->isolate());
  Context::Scope context_scope(env->context());

  if (nread <= 0)  {
    free(buf.base);
    if (nread < 0)
      stream->CallJSOnreadMethod(nread, Local<Object>());
    return;
  }

  CHECK_LE(static_cast<size_t>(nread), buf.len);

  Local<Object> obj = Buffer::New(env, buf.base, nread).ToLocalChecked();
  stream->CallJSOnreadMethod(nread, obj);
}

void EmitToJSStreamListener::CallWithStatus(Local<Name> cbname, int status) {
  CHECK_NE(stream_, nullptr);
  StreamBase* stream = static_cast<StreamBase*>(stream_);
  Environment* env = stream->stream_env();

  Local<Object> obj = stream->GetObject();
  Local<Value> cb =
      obj->Get(env->context(), cbname).ToLocalChecked();
  CHECK(cb->IsFunction());
  Local<Value> argv[] = {
    Integer::New(env->isolate(), status)
  };

  cb.As<Function>()->Call(env->context(), obj, arraysize(argv), argv)
      .ToLocalChecked();
}

void EmitToJSStreamListener::OnStreamAfterShutdown(int status) {
  Environment* env = static_cast<StreamBase*>(stream_)->stream_env();
  CallWithStatus(env->onaftershutdown_string(), status);
}

void EmitToJSStreamListener::OnStreamAfterWrite(int status) {
  Environment* env = static_cast<StreamBase*>(stream_)->stream_env();
  CallWithStatus(env->onafterwrite_string(), status);
}

}  // namespace node
