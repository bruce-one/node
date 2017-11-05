#include "node_internals.h"
#include "node_watchdog.h"

namespace node {
namespace util {

using v8::Array;
using v8::Context;
using v8::FunctionCallbackInfo;
using v8::Integer;
using v8::Local;
using v8::Maybe;
using v8::Object;
using v8::Private;
using v8::Promise;
using v8::Proxy;
using v8::String;
using v8::Value;


#define VALUE_METHOD_MAP(V)                                                   \
  V(isArray, IsArray)                                                         \
  V(isArrayBuffer, IsArrayBuffer)                                             \
  V(isAsyncFunction, IsAsyncFunction)                                         \
  V(isBooleanObject, IsBooleanObject)                                         \
  V(isDataView, IsDataView)                                                   \
  V(isDate, IsDate)                                                           \
  V(isExternal, IsExternal)                                                   \
  V(isMap, IsMap)                                                             \
  V(isMapIterator, IsMapIterator)                                             \
  V(isNativeError, IsNativeError)                                             \
  V(isNumberObject, IsNumberObject)                                           \
  V(isPromise, IsPromise)                                                     \
  V(isRegExp, IsRegExp)                                                       \
  V(isSet, IsSet)                                                             \
  V(isSetIterator, IsSetIterator)                                             \
  V(isSharedArrayBuffer, IsSharedArrayBuffer)                                 \
  V(isStringObject, IsStringObject)                                           \
  V(isSymbolObject, IsSymbolObject)                                           \
  V(isTypedArray, IsTypedArray)                                               \
  V(isUint8Array, IsUint8Array)                                               \
  V(isArrayBufferView, IsArrayBufferView)                                     \


#define V(_, ucname) \
  static void ucname(const FunctionCallbackInfo<Value>& args) {               \
    CHECK_EQ(1, args.Length());                                               \
    args.GetReturnValue().Set(args[0]->ucname());                             \
  }

  VALUE_METHOD_MAP(V)
#undef V

static void IsAnyArrayBuffer(const FunctionCallbackInfo<Value>& args) {
  CHECK_EQ(1, args.Length());
  args.GetReturnValue().Set(
    args[0]->IsArrayBuffer() || args[0]->IsSharedArrayBuffer());
}

static void GetValueKind(const FunctionCallbackInfo<Value>& args) {
  const char* name;
  Local<Value> arg = args[0];
  while (arg->IsProxy()) {
    arg = arg.As<Proxy>()->GetTarget();
  }

  do {
#define V(_, ucname) if (arg->ucname()) { name = #ucname + 2; break; }

    VALUE_METHOD_MAP(V)
#undef V
    return;
  } while (false);

  args.GetReturnValue().Set(
      String::NewFromOneByte(args.GetIsolate(),
                             reinterpret_cast<const uint8_t*>(name),
                             v8::NewStringType::kInternalized,
                             strlen(name)).ToLocalChecked());
}

static void GetPromiseDetails(const FunctionCallbackInfo<Value>& args) {
  // Return undefined if it's not a Promise.
  if (!args[0]->IsPromise())
    return;

  Environment* env = Environment::GetCurrent(args);
  Local<Context> context = env->context();

  Local<Promise> promise = args[0].As<Promise>();
  Local<Array> ret = Array::New(env->isolate(), 2);
  CHECK(ret->SetPrototype(context, Null(env->isolate())).FromJust());

  int state = promise->State();
  CHECK(ret->Set(context, 0, Integer::New(env->isolate(), state)).FromJust());
  if (state != Promise::PromiseState::kPending) {
    CHECK(ret->Set(context, 1, promise->Result()).FromJust());
  }

  args.GetReturnValue().Set(ret);
}

static void GetProxyDetails(const FunctionCallbackInfo<Value>& args) {
  // Return undefined if it's not a proxy.
  if (!args[0]->IsProxy())
    return;

  Environment* env = Environment::GetCurrent(args);
  Local<Context> context = env->context();
  Local<Proxy> proxy = args[0].As<Proxy>();

  Local<Array> ret = Array::New(env->isolate(), 2);
  CHECK(ret->SetPrototype(context, Null(env->isolate())).FromJust());
  CHECK(ret->Set(context, 0, proxy->GetTarget()).FromJust());
  CHECK(ret->Set(context, 1, proxy->GetHandler()).FromJust());

  args.GetReturnValue().Set(ret);
}

// Side effect-free stringification that will never throw exceptions.
static void SafeToString(const FunctionCallbackInfo<Value>& args) {
  auto context = args.GetIsolate()->GetCurrentContext();
  args.GetReturnValue().Set(args[0]->ToDetailString(context).ToLocalChecked());
}

inline Local<Private> IndexToPrivateSymbol(Environment* env, uint32_t index) {
#define V(name, _) &Environment::name,
  static Local<Private> (Environment::*const methods[])() const = {
    PER_ISOLATE_PRIVATE_SYMBOL_PROPERTIES(V)
  };
#undef V
  CHECK_LT(index, arraysize(methods));
  return (env->*methods[index])();
}

static void GetHiddenValue(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);

  CHECK(args[0]->IsObject());
  CHECK(args[1]->IsUint32());

  Local<Object> obj = args[0].As<Object>();
  auto index = args[1]->Uint32Value(env->context()).FromJust();
  auto private_symbol = IndexToPrivateSymbol(env, index);
  auto maybe_value = obj->GetPrivate(env->context(), private_symbol);

  args.GetReturnValue().Set(maybe_value.ToLocalChecked());
}

static void SetHiddenValue(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);

  CHECK(args[0]->IsObject());
  CHECK(args[1]->IsUint32());

  Local<Object> obj = args[0].As<Object>();
  auto index = args[1]->Uint32Value(env->context()).FromJust();
  auto private_symbol = IndexToPrivateSymbol(env, index);
  auto maybe_value = obj->SetPrivate(env->context(), private_symbol, args[2]);

  args.GetReturnValue().Set(maybe_value.FromJust());
}


void StartSigintWatchdog(const FunctionCallbackInfo<Value>& args) {
  int ret = SigintWatchdogHelper::GetInstance()->Start();
  args.GetReturnValue().Set(ret == 0);
}


void StopSigintWatchdog(const FunctionCallbackInfo<Value>& args) {
  bool had_pending_signals = SigintWatchdogHelper::GetInstance()->Stop();
  args.GetReturnValue().Set(had_pending_signals);
}


void WatchdogHasPendingSigint(const FunctionCallbackInfo<Value>& args) {
  bool ret = SigintWatchdogHelper::GetInstance()->HasPendingSignal();
  args.GetReturnValue().Set(ret);
}


void CreatePromise(const FunctionCallbackInfo<Value>& args) {
  Local<Context> context = args.GetIsolate()->GetCurrentContext();
  auto maybe_resolver = Promise::Resolver::New(context);
  if (!maybe_resolver.IsEmpty())
    args.GetReturnValue().Set(maybe_resolver.ToLocalChecked());
}


void PromiseResolve(const FunctionCallbackInfo<Value>& args) {
  Local<Context> context = args.GetIsolate()->GetCurrentContext();
  Local<Value> promise = args[0];
  CHECK(promise->IsPromise());
  if (promise.As<Promise>()->State() != Promise::kPending) return;
  Local<Promise::Resolver> resolver = promise.As<Promise::Resolver>();  // sic
  Maybe<bool> ret = resolver->Resolve(context, args[1]);
  args.GetReturnValue().Set(ret.FromMaybe(false));
}


void PromiseReject(const FunctionCallbackInfo<Value>& args) {
  Local<Context> context = args.GetIsolate()->GetCurrentContext();
  Local<Value> promise = args[0];
  CHECK(promise->IsPromise());
  if (promise.As<Promise>()->State() != Promise::kPending) return;
  Local<Promise::Resolver> resolver = promise.As<Promise::Resolver>();  // sic
  Maybe<bool> ret = resolver->Reject(context, args[1]);
  args.GetReturnValue().Set(ret.FromMaybe(false));
}


void Initialize(Local<Object> target,
                Local<Value> unused,
                Local<Context> context) {
  Environment* env = Environment::GetCurrent(context);

#define V(lcname, ucname) env->SetMethod(target, #lcname, ucname);
  VALUE_METHOD_MAP(V)
#undef V

  env->SetMethod(target, "isAnyArrayBuffer", IsAnyArrayBuffer);

#define V(name, _)                                                            \
  target->Set(context,                                                        \
              FIXED_ONE_BYTE_STRING(env->isolate(), #name),                   \
              Integer::NewFromUnsigned(env->isolate(), index++)).FromJust();
  {
    uint32_t index = 0;
    PER_ISOLATE_PRIVATE_SYMBOL_PROPERTIES(V)
  }
#undef V

  target->DefineOwnProperty(
    env->context(),
    OneByteString(env->isolate(), "pushValToArrayMax"),
    Integer::NewFromUnsigned(env->isolate(), NODE_PUSH_VAL_TO_ARRAY_MAX),
    v8::ReadOnly).FromJust();

#define V(name)                                                               \
  target->Set(context,                                                        \
              FIXED_ONE_BYTE_STRING(env->isolate(), #name),                   \
              Integer::New(env->isolate(), Promise::PromiseState::name))      \
    .FromJust()
  V(kPending);
  V(kFulfilled);
  V(kRejected);
#undef V

  env->SetMethod(target, "getHiddenValue", GetHiddenValue);
  env->SetMethod(target, "setHiddenValue", SetHiddenValue);
  env->SetMethod(target, "getPromiseDetails", GetPromiseDetails);
  env->SetMethod(target, "getProxyDetails", GetProxyDetails);
  env->SetMethod(target, "getValueKind", GetValueKind);
  env->SetMethod(target, "safeToString", SafeToString);

  env->SetMethod(target, "startSigintWatchdog", StartSigintWatchdog);
  env->SetMethod(target, "stopSigintWatchdog", StopSigintWatchdog);
  env->SetMethod(target, "watchdogHasPendingSigint", WatchdogHasPendingSigint);

  env->SetMethod(target, "createPromise", CreatePromise);
  env->SetMethod(target, "promiseResolve", PromiseResolve);
  env->SetMethod(target, "promiseReject", PromiseReject);
}

}  // namespace util
}  // namespace node

NODE_MODULE_CONTEXT_AWARE_BUILTIN(util, node::util::Initialize)
