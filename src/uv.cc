#include "uv.h"
#include "node.h"
#include "env.h"
#include "string_bytes.h"
#include "env-inl.h"

namespace node {
namespace uv {

using v8::Context;
using v8::FunctionCallbackInfo;
using v8::FunctionTemplate;
using v8::Integer;
using v8::Local;
using v8::Object;
using v8::String;
using v8::Value;


void ErrName(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  int err = args[0]->Int32Value();
  if (err >= 0)
    return env->ThrowError("err >= 0");
  const char* name = uv_err_name(err);
  args.GetReturnValue().Set(OneByteString(env->isolate(), name));
}

extern "C" {

UV_EXTERN
ssize_t path_posix_resolve(const uv_buf_t* from, const uv_buf_t* to,
    uv_buf_t* res, size_t* resolved_device_length_);
UV_EXTERN
ssize_t path_win32_resolve(const uv_buf_t* from, const uv_buf_t* to,
    uv_buf_t* res, size_t* resolved_device_length_);
}

void PathPosixResolve(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);

  BufferValue from(env->isolate(), args[0]);
  BufferValue to(env->isolate(), args[1]);
  uv_buf_t from_, to_;
  from_.len = from.length();
  from_.base = *from;
  to_.len = to.length();
  to_.base = *to;
  uv_buf_t res;
  res.base = NULL;
  ssize_t r = path_posix_resolve(&from_, &to_, &res, nullptr);
  if (r < 0) {
    free(res.base);
    return env->ThrowUVException(r,
                                 "path_posix_resolve",
                                 *from,
                                 *to);
  }

  Local<Value> rc = StringBytes::Encode(env->isolate(),
                                        res.base,
                                        res.len,
                                        UTF8);

  free(res.base);
  if (rc.IsEmpty()) {
    return env->ThrowUVException(UV_EINVAL,
                                 "path_posix_resolve",
                                 "Invalid character encoding for path",
                                 *from);
  }
  args.GetReturnValue().Set(rc);
}

void PathWin32Resolve(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);

  BufferValue from(env->isolate(), args[0]);
  BufferValue to(env->isolate(), args[1]);
  uv_buf_t from_, to_;
  from_.len = from.length();
  from_.base = *from;
  to_.len = to.length();
  to_.base = *to;
  uv_buf_t res;
  res.base = NULL;
  ssize_t r = path_win32_resolve(&from_, &to_, &res, nullptr);
  if (r < 0) {
    free(res.base);
    return env->ThrowUVException(r,
                                 "path_win32_resolve",
                                 *from,
                                 *to);
  }

  Local<Value> rc = StringBytes::Encode(env->isolate(),
                                        res.base,
                                        res.len,
                                        UTF8);

  free(res.base);
  if (rc.IsEmpty()) {
    return env->ThrowUVException(UV_EINVAL,
                                 "path_win32_resolve",
                                 "Invalid character encoding for path",
                                 *from);
  }
  args.GetReturnValue().Set(rc);
}

void Initialize(Local<Object> target,
                Local<Value> unused,
                Local<Context> context) {
  Environment* env = Environment::GetCurrent(context);
  target->Set(FIXED_ONE_BYTE_STRING(env->isolate(), "errname"),
              env->NewFunctionTemplate(ErrName)->GetFunction());
  target->Set(FIXED_ONE_BYTE_STRING(env->isolate(), "path_win32_resolve"),
              env->NewFunctionTemplate(PathWin32Resolve)->GetFunction());
  target->Set(FIXED_ONE_BYTE_STRING(env->isolate(), "path_posix_resolve"),
              env->NewFunctionTemplate(PathPosixResolve)->GetFunction());
#define V(name, _)                                                            \
  target->Set(FIXED_ONE_BYTE_STRING(env->isolate(), "UV_" # name),            \
              Integer::New(env->isolate(), UV_ ## name));
  UV_ERRNO_MAP(V)
#undef V
}


}  // namespace uv
}  // namespace node

NODE_MODULE_CONTEXT_AWARE_BUILTIN(uv, node::uv::Initialize)
