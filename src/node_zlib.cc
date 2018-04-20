// Copyright Joyent, Inc. and other Node contributors.
//
// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the
// "Software"), to deal in the Software without restriction, including
// without limitation the rights to use, copy, modify, merge, publish,
// distribute, sublicense, and/or sell copies of the Software, and to permit
// persons to whom the Software is furnished to do so, subject to the
// following conditions:
//
// The above copyright notice and this permission notice shall be included
// in all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
// OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN
// NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
// DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
// OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
// USE OR OTHER DEALINGS IN THE SOFTWARE.

#include "node.h"
#include "node_buffer.h"
#include "node_internals.h"

#include "req_wrap-inl.h"
#include "stream_base-inl.h"
#include "env-inl.h"
#include "util-inl.h"

#include "v8.h"
#include "zlib.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

namespace node {

using v8::Array;
using v8::ArrayBuffer;
using v8::Context;
using v8::Function;
using v8::FunctionCallbackInfo;
using v8::FunctionTemplate;
using v8::HandleScope;
using v8::Int8Array;
using v8::Int32;
using v8::Local;
using v8::Number;
using v8::Object;
using v8::String;
using v8::Value;

namespace {

enum class ZlibMode {
  NONE,
  DEFLATE,
  INFLATE,
  GZIP,
  GUNZIP,
  DEFLATERAW,
  INFLATERAW,
  UNZIP,
  MAX_MODE_VALUE = UNZIP
};

const uint8_t kGzipHeaderMagicBytes[] = { 0x1f, 0x8b };

/*
 * This class provides a simple compression interface, either compressing
 * or uncompressing input data.
 *
 * It is explicitly set to synchronous or asynchronous mode. In asynchronous
 * mode, work if offloaded to the threadpool through uv_work_t.
 * In synchronous mode, data will be made available while input is being read.
 */
class ZlibStream : public StreamBase, public AsyncWrap {
 public:
  ZlibStream(Environment* env, Local<Object> wrap);
  ~ZlibStream() override;

  void Close();

  WriteWrap* CreateWriteWrap(Local<Object> object) override;

  int DoTryWrite(uv_buf_t** bufs, size_t* count) override;
  int DoWrite(WriteWrap* w,
              uv_buf_t* bufs,
              size_t count,
              uv_stream_t* send_handle) override;
  int DoShutdown(ShutdownWrap* shutdown_wrap) override;

  int ReadStart() override;
  int ReadStop() override;
  bool IsAlive() override;
  bool IsClosing() override;
  AsyncWrap* GetAsyncWrap() override { return this; }

  const char* Error() const override;
  void ClearError() override;


  static void New(const FunctionCallbackInfo<Value>& args);
  static void Close(const FunctionCallbackInfo<Value>& args);
  static void SetDictionary(const FunctionCallbackInfo<Value>& args);
  static void UpdateParameters(const FunctionCallbackInfo<Value>& args);
  static void Reset(const FunctionCallbackInfo<Value>& args);

  size_t self_size() const override {
    return sizeof(*this) + dictionary_.size();
  }

  enum Flags {
    kFlushFlag,
    kLevel,
    kMemLevel,
    kMode,
    kStrategy,
    kWindowBits,
    kIsAsync,
    kOptionFieldCount
  };

 private:
  typedef SimpleWriteWrap<ReqWrap<uv_work_t>> ZlibWriteWrap;

  inline ZlibMode mode() const {
    return static_cast<ZlibMode>(options_[kMode]);
  }

  inline void set_mode(ZlibMode new_mode) {
    options_[kMode] = static_cast<int8_t>(new_mode);
    CHECK_EQ(mode(), new_mode);
  }

  void Process();
  int After(int status);
  void SetDictionary();
  void Init();
  void Reset();
  void UpdateParameters();
  void DispatchWork();

  static ZlibStream* from_uv_work_t(uv_work_t* work_req);

  static const int64_t kDeflateContextSize = 16384;  // approximate
  static const int64_t kInflateContextSize = 10240;  // approximate

  z_stream strm_;
  std::vector<char> dictionary_;
  int err_ = Z_OK;
  int8_t options_[kOptionFieldCount] = {};
  bool pending_close_ = false;
  bool init_done_ = false;
  bool reading_ = false;
  short gzip_id_bytes_read_ = 0;
  uv_buf_t output_buffer_ = uv_buf_init(nullptr, 0);
  std::vector<uv_buf_t> input_buffers_;
  ZlibWriteWrap* current_write_ = nullptr;
  const char* error_ = nullptr;
};

ZlibStream::ZlibStream(Environment* env, Local<Object> wrap)
    : StreamBase(env), AsyncWrap(env, wrap, AsyncWrap::PROVIDER_ZLIB) {
  MakeWeak<ZlibStream>(this);
  options_[kFlushFlag] = Z_NO_FLUSH;
  set_mode(ZlibMode::NONE);

  Local<ArrayBuffer> ab =
      ArrayBuffer::New(env->isolate(), options_, sizeof(options_));
  Local<Int8Array> typed_array =
      Int8Array::New(ab, 0, sizeof(options_));
  wrap->Set(env->context(), env->options_string(), typed_array).FromJust();
}


ZlibStream::~ZlibStream() {
  CHECK_EQ(current_write_, nullptr);
  Close();
}

void ZlibStream::Close() {
  if (current_write_ != nullptr) {
    pending_close_ = true;
    return;
  }

  pending_close_ = false;
  CHECK_LE(static_cast<int>(mode()),
           static_cast<int>(ZlibMode::MAX_MODE_VALUE));

  int status = Z_OK;
  int64_t change_in_bytes = 0;
  switch (mode()) {
    case ZlibMode::DEFLATE:
    case ZlibMode::DEFLATERAW:
    case ZlibMode::GZIP:
      status = deflateEnd(&strm_);
      change_in_bytes = -kDeflateContextSize;
      break;
    case ZlibMode::INFLATE:
    case ZlibMode::INFLATERAW:
    case ZlibMode::GUNZIP:
    case ZlibMode::UNZIP:
      status = inflateEnd(&strm_);
      change_in_bytes = -kInflateContextSize;
      break;
    case ZlibMode::NONE:
      break;
  }
  env()->isolate()->AdjustAmountOfExternalAllocatedMemory(change_in_bytes);
  CHECK(status == Z_OK || status == Z_DATA_ERROR);
  set_mode(ZlibMode::NONE);

  dictionary_.clear();
}

int ZlibStream::DoTryWrite(uv_buf_t** bufs, size_t* count) {
  if (options_[kIsAsync])
    return 0;
  int ret = DoWrite(nullptr, *bufs, *count, nullptr);
  if (ret == 0) {
    *bufs += *count;
    *count = 0;
  }
  return ret;
}

WriteWrap* ZlibStream::CreateWriteWrap(Local<Object> object) {
  return options_[kIsAsync] ? new ZlibWriteWrap(this, object) : nullptr;
}

ZlibStream* ZlibStream::from_uv_work_t(uv_work_t* work_req) {
  ZlibWriteWrap* write_wrap = static_cast<ZlibWriteWrap*>(
      ReqWrap<uv_work_t>::from_req(work_req));
  return static_cast<ZlibStream*>(write_wrap->stream());
}

int ZlibStream::DoWrite(WriteWrap* w,
                        uv_buf_t* bufs,
                        size_t count,
                        uv_stream_t* send_handle) {
  if (!init_done_)
    Init();
  CHECK(mode() != ZlibMode::NONE && "already finalized");

  CHECK(current_write_ == nullptr && "write already in progress");
  CHECK(!pending_close_ && "close is pending");
  current_write_ = static_cast<ZlibWriteWrap*>(w);

  int flush = options_[kFlushFlag];
  if (flush != Z_NO_FLUSH &&
      flush != Z_PARTIAL_FLUSH &&
      flush != Z_SYNC_FLUSH &&
      flush != Z_FULL_FLUSH &&
      flush != Z_FINISH &&
      flush != Z_BLOCK) {
    CHECK(0 && "Invalid flush value");
  }

  // TODO(addaleax): make this depend on the bytes read/written ratio
  output_buffer_ = EmitAlloc(65536);

  input_buffers_.insert(input_buffers_.end(),
                        bufs,
                        bufs + count);

  if (!options_[kIsAsync]) {
    // sync version
    env()->PrintSyncTrace();
    Process();
    return After(0);
  }

  if (reading_)
    DispatchWork();
  return 0;
}

void ZlibStream::DispatchWork() {
  // async version
  uv_queue_work(env()->event_loop(),
                current_write_->req(),
                [](uv_work_t* work_req) {
    from_uv_work_t(work_req)->Process();
  }, [](uv_work_t* work_req, int status) {
    ZlibStream* stream = from_uv_work_t(work_req);
    HandleScope handle_scope(stream->env()->isolate());
    Context::Scope context_scope(stream->env()->context());
    stream->After(status);
  });
}

int ZlibStream::DoShutdown(ShutdownWrap* shutdown_wrap) {
  Close();
  shutdown_wrap->Done(0);
  return 0;
}

int ZlibStream::ReadStart() {
  if (!reading_ && current_write_ != nullptr)
    DispatchWork();
  reading_ = true;
  return 0;
}

int ZlibStream::ReadStop() {
  reading_ = false;
  return 0;
}

bool ZlibStream::IsAlive() {
  return mode() != ZlibMode::NONE;
}

bool ZlibStream::IsClosing() {
  return pending_close_;
}

// thread pool!
// This function may be called multiple times on the uv_work pool
// for a single write() call, until all of the input bytes have
// been consumed.
void ZlibStream::Process() {
  const Bytef* next_expected_header_byte = nullptr;
  const int flush = options_[kFlushFlag];

  // If the avail_out is left at 0, then it means that it ran out
  // of room.  If there was avail_out left over, then it means
  // that all of the input was consumed.
  switch (mode()) {
    case ZlibMode::DEFLATE:
    case ZlibMode::GZIP:
    case ZlibMode::DEFLATERAW:
      err_ = deflate(&strm_, flush);
      break;
    case ZlibMode::UNZIP:
      if (strm_.avail_in > 0) {
        next_expected_header_byte = strm_.next_in;
      }

      switch (gzip_id_bytes_read_) {
        case 0:
          if (next_expected_header_byte == nullptr) {
            break;
          }

          if (*next_expected_header_byte == kGzipHeaderMagicBytes[0]) {
            gzip_id_bytes_read_ = 1;
            next_expected_header_byte++;

            if (strm_.avail_in == 1) {
              // The only available byte was already read.
              break;
            }
          } else {
            set_mode(ZlibMode::INFLATE);
            break;
          }

          // fallthrough
        case 1:
          if (next_expected_header_byte == nullptr) {
            break;
          }

          if (*next_expected_header_byte == kGzipHeaderMagicBytes[1]) {
            gzip_id_bytes_read_ = 2;
            set_mode(ZlibMode::GUNZIP);
          } else {
            // There is no actual difference between INFLATE and INFLATERAW
            // (after initialization).
            set_mode(ZlibMode::INFLATE);
          }

          break;
        default:
          CHECK(0 && "invalid number of gzip magic number bytes read");
      }

      // fallthrough
    case ZlibMode::INFLATE:
    case ZlibMode::GUNZIP:
    case ZlibMode::INFLATERAW:
      err_ = inflate(&strm_, flush);

      // If data was encoded with dictionary (INFLATERAW will have it set in
      // SetDictionary, don't repeat that here)
      if (mode() != ZlibMode::INFLATERAW &&
          err_ == Z_NEED_DICT &&
          !dictionary_.empty()) {
        // Load it
        err_ = inflateSetDictionary(
            &strm_,
            reinterpret_cast<const Bytef*>(dictionary_.data()),
            dictionary_.size());
        if (err_ == Z_OK) {
          // And try to decode again
          err_ = inflate(&strm_, flush);
        } else if (err_ == Z_DATA_ERROR) {
          // Both inflateSetDictionary() and inflate() return Z_DATA_ERROR.
          // Make it possible for After() to tell a bad dictionary from bad
          // input.
          err_ = Z_NEED_DICT;
        }
      }

      while (strm_.avail_in > 0 &&
             mode() == ZlibMode::GUNZIP &&
             err_ == Z_STREAM_END &&
             strm_.next_in[0] != 0x00) {
        // Bytes remain in input buffer. Perhaps this is another compressed
        // member in the same archive, or just trailing garbage.
        // Trailing zero bytes are okay, though, since they are frequently
        // used for padding.

        Reset();
        err_ = inflate(&strm_, flush);
      }
      break;
    default:
      UNREACHABLE();
  }

  // pass any errors back to the main thread to deal with.

  // now After will emit the output, and
  // either schedule another call to Process,
  // or shift the queue and call Process.
}

const char* ZlibStream::Error() const {
  if (error_ != nullptr)
    return error_;
  // Acceptable error states depend on the type of zlib stream.
  switch (err_) {
    case Z_OK:
    case Z_BUF_ERROR:
      if (strm_.avail_out != 0 && options_[kFlushFlag] == Z_FINISH) {
        return "unexpected end of file";
      }
      // fall through
    case Z_STREAM_END:
      // normal statuses, not fatal
      return nullptr;
    case Z_NEED_DICT:
      if (dictionary_.empty())
        return "Missing dictionary";
      else
        return "Bad dictionary";
    default:
      // something else.
      return "Zlib error";
  }
}

void ZlibStream::ClearError() {
  err_ = Z_OK;
  options_[kFlushFlag] = Z_NO_FLUSH;
}

// v8 land!
int ZlibStream::After(int status) {
  CHECK_EQ(status, 0);

  int stream_err = Error() ? UV_EPROTO : 0;

  EmitRead(stream_err, output_buffer_);

  if (current_write_ != nullptr) {
    WriteWrap* wrap = current_write_;
    current_write_ = nullptr;
    wrap->Done(stream_err);
  }

  if (pending_close_)
    Close();

  return stream_err;
}

void ZlibStream::New(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  new ZlibStream(env, args.This());
}

void ZlibStream::Close(const FunctionCallbackInfo<Value>& args) {
  ZlibStream* stream;
  ASSIGN_OR_RETURN_UNWRAP(&stream, args.Holder());
  stream->Close();
}

void ZlibStream::SetDictionary(const FunctionCallbackInfo<Value>& args) {
  ZlibStream* stream;
  ASSIGN_OR_RETURN_UNWRAP(&stream, args.Holder());

  CHECK(args[0]->IsUint8Array());
  const char* dictionary = Buffer::Data(args[0]);
  size_t length = Buffer::Length(args[0]);
  stream->dictionary_ = std::vector<char>(dictionary, dictionary + length);

  stream->SetDictionary();
}

void ZlibStream::UpdateParameters(const FunctionCallbackInfo<Value>& args) {
  ZlibStream* stream;
  ASSIGN_OR_RETURN_UNWRAP(&stream, args.Holder());

  stream->UpdateParameters();
}

void ZlibStream::Reset(const FunctionCallbackInfo<Value> &args) {
  ZlibStream* stream;
  ASSIGN_OR_RETURN_UNWRAP(&stream, args.Holder());

  stream->Reset();
}

void ZlibStream::Init() {
  strm_.zalloc = Z_NULL;
  strm_.zfree = Z_NULL;
  strm_.opaque = Z_NULL;

  /*if (ctx->mode_ == GZIP || ctx->mode_ == GUNZIP) {
    ctx->windowBits_ += 16;
  }

  if (ctx->mode_ == UNZIP) {
    ctx->windowBits_ += 32;
  }

  if (ctx->mode_ == DEFLATERAW || ctx->mode_ == INFLATERAW) {
    ctx->windowBits_ *= -1;
  }*/

  switch (mode()) {
    case ZlibMode::DEFLATE:
    case ZlibMode::GZIP:
    case ZlibMode::DEFLATERAW:
      err_ = deflateInit2(&strm_,
                          options_[kLevel],
                          Z_DEFLATED,
                          options_[kWindowBits],
                          options_[kMemLevel],
                          options_[kStrategy]);
      env()->isolate()
          ->AdjustAmountOfExternalAllocatedMemory(kDeflateContextSize);
      break;
    case ZlibMode::INFLATE:
    case ZlibMode::GUNZIP:
    case ZlibMode::INFLATERAW:
    case ZlibMode::UNZIP:
      err_ = inflateInit2(&strm_, options_[kWindowBits]);
      env()->isolate()
          ->AdjustAmountOfExternalAllocatedMemory(kInflateContextSize);
      break;
    default:
      UNREACHABLE();
  }

  init_done_ = true;
}

void ZlibStream::SetDictionary() {
  if (dictionary_.empty())
    return;

  err_ = Z_OK;

  switch (mode()) {
    case ZlibMode::DEFLATE:
    case ZlibMode::DEFLATERAW:
      err_ = deflateSetDictionary(
          &strm_,
          reinterpret_cast<const Bytef*>(dictionary_.data()),
          dictionary_.size());
      break;
    case ZlibMode::INFLATERAW:
      // The other inflate cases will have the dictionary set when inflate()
      // returns Z_NEED_DICT in Process()
      err_ = inflateSetDictionary(
          &strm_,
          reinterpret_cast<const Bytef*>(dictionary_.data()),
          dictionary_.size());
      break;
    default:
      break;
  }

  if (err_ != Z_OK) {
    error_ = "Failed to set dictionary";
  }
}

void ZlibStream::UpdateParameters() {
  err_ = Z_OK;

  switch (mode()) {
    case ZlibMode::DEFLATE:
    case ZlibMode::DEFLATERAW:
      err_ = deflateParams(&strm_, options_[kLevel], options_[kStrategy]);
      break;
    default:
      break;
  }

  if (err_ != Z_OK && err_ != Z_BUF_ERROR) {
    error_ =  "Failed to set parameters";
  }
}

void ZlibStream::Reset() {
  err_ = Z_OK;

  switch (mode()) {
    case ZlibMode::DEFLATE:
    case ZlibMode::DEFLATERAW:
    case ZlibMode::GZIP:
      err_ = deflateReset(&strm_);
      break;
    case ZlibMode::INFLATE:
    case ZlibMode::INFLATERAW:
    case ZlibMode::GUNZIP:
      err_ = inflateReset(&strm_);
      break;
    default:
      break;
  }

  if (err_ != Z_OK) {
    error_ =  "Failed to reset stream";
  }
}

void Initialize(Local<Object> target,
                Local<Value> unused,
                Local<Context> context,
                void* priv) {
  Environment* env = Environment::GetCurrent(context);
  Local<FunctionTemplate> z = env->NewFunctionTemplate(ZlibStream::New);

  z->InstanceTemplate()->SetInternalFieldCount(1);

  AsyncWrap::AddWrapMethods(env, z);
  StreamBase::AddMethods<ZlibStream>(env, z, StreamBase::kFlagHasWritev);
  env->SetProtoMethod(z, "setDictionary", ZlibStream::SetDictionary);
  env->SetProtoMethod(z, "updateParameters", ZlibStream::UpdateParameters);
  env->SetProtoMethod(z, "close", ZlibStream::Close);
  env->SetProtoMethod(z, "reset", ZlibStream::Reset);

  Local<String> zlibString = FIXED_ONE_BYTE_STRING(env->isolate(), "Zlib");
  z->SetClassName(zlibString);
  target->Set(env->context(),
              zlibString,
              z->GetFunction(env->context()).ToLocalChecked()).FromJust();

  target->Set(env->context(),
              FIXED_ONE_BYTE_STRING(env->isolate(), "ZLIB_VERSION"),
              FIXED_ONE_BYTE_STRING(env->isolate(), ZLIB_VERSION)).FromJust();

#define ZLIB_OPTION_FIELD(name) do {                 \
    const int name = ZlibStream::name;               \
    NODE_DEFINE_CONSTANT(target, name);              \
  } while(0)

  ZLIB_OPTION_FIELD(kFlushFlag);
  ZLIB_OPTION_FIELD(kLevel);
  ZLIB_OPTION_FIELD(kMemLevel);
  ZLIB_OPTION_FIELD(kMode);
  ZLIB_OPTION_FIELD(kStrategy);
  ZLIB_OPTION_FIELD(kWindowBits);
  ZLIB_OPTION_FIELD(kIsAsync);
  ZLIB_OPTION_FIELD(kOptionFieldCount);
}

}  // anonymous namespace
}  // namespace node

NODE_MODULE_CONTEXT_AWARE_INTERNAL(zlib, node::Initialize)
