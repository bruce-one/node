#include "node_worker.h"
#include "node_errors.h"
#include "node_internals.h"
#include "node_buffer.h"
#include "node_perf.h"
#include "util.h"
#include "util-inl.h"
#include "async_wrap.h"
#include "async_wrap-inl.h"

using v8::ArrayBuffer;
using v8::Context;
using v8::Function;
using v8::FunctionCallbackInfo;
using v8::FunctionTemplate;
using v8::HandleScope;
using v8::Integer;
using v8::Isolate;
using v8::Local;
using v8::Locker;
using v8::Number;
using v8::Object;
using v8::SealHandleScope;
using v8::String;
using v8::Value;

namespace node {
namespace worker {

namespace {

double next_thread_id = 1;
Mutex next_thread_id_mutex;

struct ChildListener : public MessagePort::FlaggedMessageListener {
  explicit ChildListener(Environment* e) : env(e) {}
  ~ChildListener() {}

  void HandleMessage(MessageFlag flag) override {
    // The child context only understands stopping messages right now.
    CHECK_EQ(flag, kMessageFlagStopThreadOrder);
    uv_stop(env->event_loop());
  }

  Environment* env;  // The child's Environment.
};

struct ParentListener : public MessagePort::FlaggedMessageListener {
  explicit ParentListener(Worker* worker) : w(worker) {}
  ~ParentListener() {}

  void HandleMessage(MessageFlag flag) override {
    // The child context only understands stop state indicators right now.
    CHECK_EQ(flag, kMessageFlagThreadStopped);
    w->OnThreadStopped();
  }

  Worker* w;
};

}  // anonymous namespace

Mutex Worker::by_isolate_mutex_;
std::unordered_map<v8::Isolate*, Worker*> Worker::by_isolate_;

Worker* Worker::ForIsolate(Isolate* isolate) {
  Mutex::ScopedLock lock(by_isolate_mutex_);
  return by_isolate_[isolate];
}

Worker::Worker(Environment* env, Local<Object> wrap)
    : AsyncWrap(env, wrap, AsyncWrap::PROVIDER_WORKER) {
  MakeWeak();

  // Generate a new thread id.
  {
    Mutex::ScopedLock next_thread_id_lock(next_thread_id_mutex);
    thread_id_ = next_thread_id++;
  }
  wrap->Set(env->context(),
            env->thread_id_string(),
            Number::New(env->isolate(), thread_id_)).FromJust();

  // Set up everything that needs to be set up in the parent environment.
  std::unique_ptr<MessagePort::FlaggedMessageListener> listener
      { new ParentListener(this) };
  parent_port_ = MessagePort::New(env, env->context(), std::move(listener));
  if (parent_port_ == nullptr) {
    // This can happen e.g. because execution is terminating.
    return;
  }
  parent_port_->MarkAsPrivileged();
  parent_port_->DoNotCloseWhenSiblingCloses();

  child_port_data_.reset(new MessagePortData(nullptr));
  MessagePort::Entangle(parent_port_, child_port_data_.get());

  object()->Set(env->context(),
                env->message_port_string(),
                parent_port_->object()).FromJust();

  array_buffer_allocator_.reset(CreateArrayBufferAllocator());

  isolate_ = NewIsolate(array_buffer_allocator_.get());
  CHECK_NE(isolate_, nullptr);
  CHECK_EQ(uv_loop_init(&loop_), 0);

  {
    Mutex::ScopedLock lock(by_isolate_mutex_);
    by_isolate_[isolate_] = this;
  }

  {
    // Enter an environment capable of executing code in the child Isolate
    // (and only in it).
    Locker locker(isolate_);
    Isolate::Scope isolate_scope(isolate_);
    HandleScope handle_scope(isolate_);
    Local<Context> context = NewContext(isolate_);
    Context::Scope context_scope(context);

    isolate_data_.reset(CreateIsolateData(isolate_,
                                          &loop_,
                                          env->isolate_data()->platform(),
                                          array_buffer_allocator_.get()));
    CHECK(isolate_data_);

    // TODO(addaleax): Use CreateEnvironment()
    env_.reset(new Environment(isolate_data_.get(),
                               context,
                               nullptr));
    CHECK_NE(env_, nullptr);
    env_->set_abort_on_uncaught_exception(false);
    env_->set_worker_context(this);
    env_->set_thread_id(thread_id_);

    env_->Start(0, nullptr, 0, nullptr, env->profiler_idle_notifier_started());
  }

  // The new isolate won't be bothered on this thread again.
  isolate_->DiscardThreadSpecificMetadata();
}

bool Worker::is_stopped() const {
  Mutex::ScopedLock stopped_lock(stopped_mutex_);
  return stopped_;
}

void Worker::Run() {
  MultiIsolatePlatform* platform = isolate_data_->platform();
  CHECK_NE(platform, nullptr);

  {
    Locker locker(isolate_);
    Isolate::Scope isolate_scope(isolate_);
    SealHandleScope outer_seal(isolate_);

    {
      Context::Scope context_scope(env_->context());
      HandleScope handle_scope(isolate_);

      {
        HandleScope handle_scope(isolate_);
        Mutex::ScopedLock lock(mutex_);
        // Set up the message channel for receiving messages in the child.
        std::unique_ptr<MessagePort::FlaggedMessageListener> child_listener
            { new ChildListener(env_.get()) };
        child_port_ = MessagePort::New(env_.get(),
                                       env_->context(),
                                       std::move(child_listener),
                                       std::move(child_port_data_));
        CHECK_NE(child_port_, nullptr);
        child_port_->MarkAsPrivileged();
        env_->set_message_port(child_port_->object(isolate_));
      }

      {
        HandleScope handle_scope(isolate_);
        Environment::AsyncCallbackScope callback_scope(env_.get());
        env_->async_hooks()->push_async_ids(1, 0);
        // This loads the Node bootstrapping code.
        LoadEnvironment(env_.get());
        env_->async_hooks()->pop_async_id(1);
      }

      {
        SealHandleScope seal(isolate_);
        bool more;
        env_->performance_state()->Mark(
            node::performance::NODE_PERFORMANCE_MILESTONE_LOOP_START);
        do {
          if (is_stopped()) break;
          uv_run(&loop_, UV_RUN_DEFAULT);
          if (is_stopped()) break;

          platform->DrainBackgroundTasks(isolate_);

          more = uv_loop_alive(&loop_);
          if (more && !is_stopped())
            continue;

          EmitBeforeExit(env_.get());

          // Emit `beforeExit` if the loop became alive either after emitting
          // event, or after running some callbacks.
          more = uv_loop_alive(&loop_);
        } while (more == true);
        env_->performance_state()->Mark(
            node::performance::NODE_PERFORMANCE_MILESTONE_LOOP_EXIT);
      }
    }

    {
      int exit_code;
      bool stopped = is_stopped();
      if (!stopped)
        exit_code = EmitExit(env_.get());
      Mutex::ScopedLock lock(mutex_);
      if (exit_code_ == 0 && !stopped)
        exit_code_ = exit_code;
    }

    env_->set_can_call_into_js(false);
    Isolate::DisallowJavascriptExecutionScope disallow_js(isolate_,
        Isolate::DisallowJavascriptExecutionScope::THROW_ON_FAILURE);

    // Grab the parent-to-child channel and render is unusable.
    MessagePort* child_port;
    {
      Mutex::ScopedLock lock(mutex_);
      child_port = child_port_;
      child_port_ = nullptr;
    }

    {
      Context::Scope context_scope(env_->context());
      child_port->Close();
      env_->stop_sub_worker_contexts();
      env_->RunCleanup();
      RunAtExit(env_.get());

      {
        Mutex::ScopedLock stopped_lock(stopped_mutex_);
        stopped_ = true;
      }

      env_->RunCleanup();

      // This call needs to be made while the `Environment` is still alive
      // because we assume that it is available for async tracking in the
      // NodePlatform implementation.
      platform->DrainBackgroundTasks(isolate_);
    }

    env_.reset();
  }

  DisposeIsolate();

  // Need to run the loop one more time to close the platform's uv_async_t
  uv_run(&loop_, UV_RUN_ONCE);

  {
    Mutex::ScopedLock lock(mutex_);
    CHECK_NE(parent_port_, nullptr);
    parent_port_->AddToIncomingQueue(Message(kMessageFlagThreadStopped));
  }
}

void Worker::DisposeIsolate() {
  if (isolate_ == nullptr)
    return;

  isolate_->Dispose();

  CHECK(isolate_data_);
  MultiIsolatePlatform* platform = isolate_data_->platform();
  platform->CancelPendingDelayedTasks(isolate_);

  isolate_data_.reset();

  {
    Mutex::ScopedLock lock(by_isolate_mutex_);
    by_isolate_.erase(isolate_);
  }

  isolate_ = nullptr;
}

void Worker::JoinThread() {
  if (thread_joined_)
    return;
  CHECK_EQ(uv_thread_join(&tid_), 0);
  thread_joined_ = true;

  env()->remove_sub_worker_context(this);
}

void Worker::OnThreadStopped() {
  Mutex::ScopedLock lock(mutex_);
  {
    Mutex::ScopedLock stopped_lock(stopped_mutex_);
    CHECK(stopped_);
  }
  CHECK_EQ(child_port_, nullptr);
  parent_port_->Close();
  parent_port_ = nullptr;

  // It's okay to join the thread while holding the mutex because
  // OnThreadStopped means it's no longer doing any work that might grab it.
  JoinThread();

  {
    HandleScope handle_scope(env()->isolate());
    Context::Scope context_scope(env()->context());

    // Reset the parent port as we're closing it now anyway.
    object()->Set(env()->context(),
                  env()->message_port_string(),
                  Undefined(env()->isolate())).FromJust();

    Local<Value> code = Integer::New(env()->isolate(), exit_code_);
    MakeCallback(env()->onexit_string(), 1, &code);
  }
}

Worker::~Worker() {
  Mutex::ScopedLock lock(mutex_);
  JoinThread();

  CHECK(stopped_);
  CHECK(thread_joined_);
  CHECK_EQ(child_port_, nullptr);
  CHECK_EQ(uv_loop_close(&loop_), 0);

  DisposeIsolate();
}

void Worker::New(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);

  CHECK(args.IsConstructCall());

  if (env->isolate_data()->platform() == nullptr) {
    THROW_ERR_MISSING_PLATFORM_FOR_WORKER(env);
    return;
  }

  new Worker(env, args.This());
}

void Worker::StartThread(const FunctionCallbackInfo<Value>& args) {
  Worker* w;
  ASSIGN_OR_RETURN_UNWRAP(&w, args.This());
  Mutex::ScopedLock lock(w->mutex_);

  w->env()->add_sub_worker_context(w);
  w->stopped_ = false;
  CHECK_EQ(uv_thread_create(&w->tid_, [](void* arg) {
    static_cast<Worker*>(arg)->Run();
  }, static_cast<void*>(w)), 0);
  w->thread_joined_ = false;
}

void Worker::StopThread(const FunctionCallbackInfo<Value>& args) {
  Worker* w;
  ASSIGN_OR_RETURN_UNWRAP(&w, args.This());

  w->Exit(1);
  w->JoinThread();
}

void Worker::Exit(int code) {
  Mutex::ScopedLock lock(mutex_);
  Mutex::ScopedLock stopped_lock(stopped_mutex_);
  if (!stopped_) {
    CHECK_NE(env_, nullptr);
    stopped_ = true;
    exit_code_ = code;
    parent_port_->Send(Message(kMessageFlagStopThreadOrder));
    isolate_->TerminateExecution();
  }
}

size_t Worker::self_size() const {
  return sizeof(*this);
}

namespace {

void GetEnvMessagePort(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  Local<Object> port = env->message_port();
  if (!port.IsEmpty()) {
    CHECK_EQ(port->CreationContext()->GetIsolate(), args.GetIsolate());
    args.GetReturnValue().Set(port);
  }
}

void InitWorker(Local<Object> target,
                Local<Value> unused,
                Local<Context> context,
                void* priv) {
  Environment* env = Environment::GetCurrent(context);

  {
    Local<FunctionTemplate> w = env->NewFunctionTemplate(Worker::New);

    w->InstanceTemplate()->SetInternalFieldCount(1);

    AsyncWrap::AddWrapMethods(env, w);
    env->SetProtoMethod(w, "startThread", Worker::StartThread);
    env->SetProtoMethod(w, "stopThread", Worker::StopThread);

    Local<String> workerString =
        FIXED_ONE_BYTE_STRING(env->isolate(), "Worker");
    w->SetClassName(workerString);
    target->Set(workerString, w->GetFunction());
  }

  env->SetMethod(target, "getEnvMessagePort", GetEnvMessagePort);

  auto thread_id_string = FIXED_ONE_BYTE_STRING(env->isolate(), "threadId");
  target->Set(env->context(),
              thread_id_string,
              Number::New(env->isolate(), env->thread_id())).FromJust();

  NODE_DEFINE_CONSTANT(target, kMessageFlagNone);
  NODE_DEFINE_CONSTANT(target, kMessageFlagCustomOffset);
}

}  // anonymous namespace

}  // namespace worker
}  // namespace node

NODE_MODULE_CONTEXT_AWARE_INTERNAL(worker, node::worker::InitWorker)
