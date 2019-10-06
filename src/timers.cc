#include "env-inl.h"
#include "util-inl.h"
#include "handle_wrap.h"
#include "base_object-inl.h"
#include "async_wrap-inl.h"
#include "v8.h"

#include <cstdint>

namespace node {
namespace {

using v8::Array;
using v8::Context;
using v8::Function;
using v8::FunctionTemplate;
using v8::FunctionCallbackInfo;
using v8::HandleScope;
using v8::Integer;
using v8::Local;
using v8::Number;
using v8::Object;
using v8::String;
using v8::Value;

class TimerWrap : public HandleWrap {
 public:
  TimerWrap(Environment* env, Local<Object> obj)
    : HandleWrap(env, obj, reinterpret_cast<uv_handle_t*>(&timer_), PROVIDER_TIMER) {
    uv_timer_init(env->event_loop(), &timer_);
  }

  static void Start(const FunctionCallbackInfo<Value>& args) {
    TimerWrap* wrap;
    ASSIGN_OR_RETURN_UNWRAP(&wrap, args.Holder());

    Local<Value> timeout = args[0];
    CHECK(timeout->IsNumber());
    double timeout_ms = timeout.As<Number>()->Value();
    uv_timer_start(&wrap->timer_, Callback, timeout_ms, 0);
  }

  static void Callback(uv_timer_t* handle) {
    TimerWrap* wrap = ContainerOf(&TimerWrap::timer_, handle);
    Environment* env = wrap->env();
    HandleScope handle_scope(env->isolate());
    Context::Scope context_scope(env->context());

    wrap->MakeCallback(env->ontimeout_string(), 0, nullptr);
  }

  static void New(const FunctionCallbackInfo<Value>& args) {
    CHECK(args.IsConstructCall());
    Environment* env = Environment::GetCurrent(args);
    new TimerWrap(env, args.This());
  }

  SET_NO_MEMORY_INFO()
  SET_MEMORY_INFO_NAME(TimerWrap)
  SET_SELF_SIZE(TimerWrap)

 private:
  uv_timer_t timer_;
};

void SetupTimers(const FunctionCallbackInfo<Value>& args) {
  CHECK(args[0]->IsFunction());
  CHECK(args[1]->IsFunction());
  auto env = Environment::GetCurrent(args);

  env->set_immediate_callback_function(args[0].As<Function>());
  env->set_timers_callback_function(args[1].As<Function>());
}

void GetLibuvNow(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  args.GetReturnValue().Set(env->GetNow());
}

void ScheduleTimer(const FunctionCallbackInfo<Value>& args) {
  auto env = Environment::GetCurrent(args);
  env->ScheduleTimer(args[0]->IntegerValue(env->context()).FromJust());
}

void ToggleTimerRef(const FunctionCallbackInfo<Value>& args) {
  Environment::GetCurrent(args)->ToggleTimerRef(args[0]->IsTrue());
}

void ToggleImmediateRef(const FunctionCallbackInfo<Value>& args) {
  Environment::GetCurrent(args)->ToggleImmediateRef(args[0]->IsTrue());
}

void Initialize(Local<Object> target,
                       Local<Value> unused,
                       Local<Context> context,
                       void* priv) {
  Environment* env = Environment::GetCurrent(context);

  env->SetMethod(target, "getLibuvNow", GetLibuvNow);
  env->SetMethod(target, "setupTimers", SetupTimers);
  env->SetMethod(target, "scheduleTimer", ScheduleTimer);
  env->SetMethod(target, "toggleTimerRef", ToggleTimerRef);
  env->SetMethod(target, "toggleImmediateRef", ToggleImmediateRef);

  target->Set(env->context(),
              FIXED_ONE_BYTE_STRING(env->isolate(), "immediateInfo"),
              env->immediate_info()->fields().GetJSArray()).Check();

  Local<FunctionTemplate> t = env->NewFunctionTemplate(TimerWrap::New);
  t->InstanceTemplate()->SetInternalFieldCount(1);
  Local<String> statWatcherString =
      FIXED_ONE_BYTE_STRING(env->isolate(), "TimerWrap");
  t->SetClassName(statWatcherString);
  t->Inherit(HandleWrap::GetConstructorTemplate(env));

  env->SetProtoMethod(t, "start", TimerWrap::Start);

  target->Set(env->context(), statWatcherString,
              t->GetFunction(env->context()).ToLocalChecked()).Check();
}


}  // anonymous namespace
}  // namespace node

NODE_MODULE_CONTEXT_AWARE_INTERNAL(timers, node::Initialize)
