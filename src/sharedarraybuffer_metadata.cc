#include "sharedarraybuffer_metadata.h"
#include "base_object.h"
#include "base_object-inl.h"
#include "node_errors.h"
#include "node_worker.h"

using v8::Context;
using v8::Function;
using v8::FunctionTemplate;
using v8::HandleScope;
using v8::Isolate;
using v8::Local;
using v8::Maybe;
using v8::MaybeLocal;
using v8::Nothing;
using v8::Object;
using v8::SharedArrayBuffer;
using v8::Value;

namespace node {
namespace worker {

namespace {

// Yield a JS constructor for SABLifetimePartner objects in the form of a
// standard API object, that has a single field for containing the raw
// SABLiftimePartner* pointer.
Local<Function> GetSABLifetimePartnerConstructor(
    Environment* env, Local<Context> context) {
  Local<FunctionTemplate> templ;
  templ = env->sab_lifetimepartner_constructor_template();
  if (!templ.IsEmpty())
    return templ->GetFunction(context).ToLocalChecked();

  templ = BaseObject::MakeLazilyInitializedJSTemplate(env);
  templ->SetClassName(FIXED_ONE_BYTE_STRING(env->isolate(),
                                            "SABLifetimePartner"));
  env->set_sab_lifetimepartner_constructor_template(templ);

  return GetSABLifetimePartnerConstructor(env, context);
}

}  // anonymous namespace

class SABLifetimePartner : public BaseObject {
 public:
  SABLifetimePartner(Environment* env,
                     Local<Object> obj,
                     SharedArrayBufferMetadataReference r)
    : BaseObject(env, obj),
      reference(r) {
    MakeWeak();

    {
      Mutex::ScopedLock lock(SharedArrayBufferMetadata::mutex_);
      r->accessing_isolates_.insert(env->isolate());
    }
  }

  ~SABLifetimePartner() {
    Mutex::ScopedLock lock(SharedArrayBufferMetadata::mutex_);
    auto it = reference->accessing_isolates_.find(env()->isolate());
    CHECK_NE(it, reference->accessing_isolates_.end());
    reference->accessing_isolates_.erase(it);
    reference->CheckAllWaitersForDeadlock(
        std::string("GC on thread ") + std::to_string(env()->thread_id()));
  }

  SharedArrayBufferMetadataReference reference;
};

SharedArrayBufferMetadataReference
SharedArrayBufferMetadata::ForSharedArrayBuffer(
    Environment* env,
    Local<Context> context,
    Local<SharedArrayBuffer> source,
    bool may_attach_new_reference) {
  Local<Value> lifetime_partner;

  if (!source->GetPrivate(context,
                          env->sab_lifetimepartner_symbol())
                              .ToLocal(&lifetime_partner)) {
    return nullptr;
  }

  if (lifetime_partner->IsObject()) {
    CHECK(source->IsExternal());
    SABLifetimePartner* partner =
        Unwrap<SABLifetimePartner>(lifetime_partner.As<Object>());
    CHECK_NE(partner, nullptr);
    return partner->reference;
  }

  if (!may_attach_new_reference)
    return nullptr;

  if (source->IsExternal()) {
    // If this is an external SharedArrayBuffer but we do not see a lifetime
    // partner object, it was not us who externalized it. In that case, there
    // is no way to serialize it, because it's unclear how the memory
    // is actually owned.
    THROW_ERR_TRANSFERRING_EXTERNALIZED_SHAREDARRAYBUFFER(env);
    return nullptr;
  }

  SharedArrayBuffer::Contents contents = source->Externalize();
  SharedArrayBufferMetadataReference r(new SharedArrayBufferMetadata(
      contents.Data(), contents.ByteLength()));
  if (r->AssignToSharedArrayBuffer(env, context, source).IsNothing())
    return nullptr;
  return r;
}

Maybe<bool> SharedArrayBufferMetadata::AssignToSharedArrayBuffer(
    Environment* env, Local<Context> context,
    Local<SharedArrayBuffer> target) {
  CHECK(target->IsExternal());
  Local<Function> ctor = GetSABLifetimePartnerConstructor(env, context);
  Local<Object> obj;
  if (!ctor->NewInstance(context).ToLocal(&obj))
    return Nothing<bool>();

  new SABLifetimePartner(env, obj, shared_from_this());
  return target->SetPrivate(context,
                            env->sab_lifetimepartner_symbol(),
                            obj);
}

SharedArrayBufferMetadata::SharedArrayBufferMetadata(void* data, size_t size)
  : data(data), size(size), debug_id_(next_debug_id_.fetch_add(1)) { }

SharedArrayBufferMetadata::~SharedArrayBufferMetadata() {
  CHECK_EQ(in_transfer_count_, 0);
  free(data);
}

MaybeLocal<SharedArrayBuffer> SharedArrayBufferMetadata::GetSharedArrayBuffer(
    Environment* env, Local<Context> context) {
  Local<SharedArrayBuffer> obj =
      SharedArrayBuffer::New(env->isolate(), data, size);

  if (AssignToSharedArrayBuffer(env, context, obj).IsNothing())
    return MaybeLocal<SharedArrayBuffer>();

  return obj;
}

void SharedArrayBufferMetadata::IncreaseInTransferCount() {
  Mutex::ScopedLock lock(mutex_);
  in_transfer_count_++;
}

void SharedArrayBufferMetadata::DecreaseInTransferCount() {
  Mutex::ScopedLock lock(mutex_);
  CHECK_NE(in_transfer_count_, 0);
  in_transfer_count_--;
  CheckAllWaitersForDeadlock("Containing message got lost before emitting");
}

bool SharedArrayBufferMetadata::CanBeWokenUp(
    Isolate* isolate,
    std::unordered_set<Isolate*>* already_visited) {
  if (already_visited != nullptr && already_visited->count(isolate) == 1)
    return false;

  SharedArrayBufferMetadataReference sab = waiting_isolates_[isolate].sab;
  CHECK(sab);
  if (sab->in_transfer_count_ > 0)
    return true;
  // Common case: There is at least one other isolate with access that
  // is not sleeping.
  for (Isolate* isolate : sab->accessing_isolates_) {
    if (waiting_isolates_.count(isolate) == 0)
      return true;
  }

  std::unordered_set<Isolate*> visited;
  if (already_visited == nullptr) already_visited = &visited;
  already_visited->insert(isolate);

  for (Isolate* isolate : sab->accessing_isolates_) {
    if (CanBeWokenUp(isolate, already_visited))
      return true;
  }
  return false;
}

void SharedArrayBufferMetadata::CheckAllWaitersForDeadlock(
    const std::string& reason) {
  std::shared_ptr<std::string> debug_info;

  for (Isolate* isolate : accessing_isolates_) {
    if (waiting_isolates_.count(isolate) == 0)
      continue;

    wait_information& info = waiting_isolates_[isolate];
    CHECK(info.sab);
    if (info.sab.get() != this)
      continue;

    if (!CanBeWokenUp(isolate)) {
      if (!debug_info)
        debug_info = GenerateDebugInfo(reason);
      info.debug_info = debug_info;
      info.wake_handle->Wake();
    }
  }
}

void SharedArrayBufferMetadata::AtomicsWaitCallback(
    Isolate::AtomicsWaitEvent event,
    Local<SharedArrayBuffer> array_buffer,
    size_t offset_in_bytes,
    int32_t value,
    double timeout_in_ms,
    Isolate::AtomicsWaitWakeHandle* wake_handle,
    void* data) {
  // Time-limited waits never count as a deadlock.
  if (timeout_in_ms != std::numeric_limits<double>::infinity()) return;

  Isolate* isolate = static_cast<Isolate*>(data);
  HandleScope handle_scope(isolate);
  Local<Context> context = isolate->GetCurrentContext();
  Environment* env = Environment::GetCurrent(context);

  SharedArrayBufferMetadataReference metadata = ForSharedArrayBuffer(
      env, context, array_buffer, false);

  Mutex::ScopedLock lock(mutex_);
  if (event == Isolate::AtomicsWaitEvent::kStartWait) {
    if (!metadata) {
      // This means that nobody else is accessing this isolate.
      // The Atomics.wait() call is either going to return immediately with
      // kNotEqual, or it would block forever.
      return wake_handle->Wake();
    }

    waiting_isolates_[isolate] = wait_information { metadata, wake_handle };

    if (!CanBeWokenUp(isolate)) {
      waiting_isolates_[isolate].debug_info = GenerateDebugInfo(
          "Cannot wake up initial call for SharedArrayBuffer " +
          std::to_string(metadata->debug_id_), metadata);
      return wake_handle->Wake();
    }
  } else {
    wait_information info = std::move(waiting_isolates_[isolate]);
    waiting_isolates_.erase(isolate);
    if (event == Isolate::AtomicsWaitEvent::kAPIStopped) {
      Context::Scope context_scope(context);
      std::shared_ptr<std::string> debug_info;
      if (info.debug_info)
        debug_info = std::move(info.debug_info);
      else
        debug_info = GenerateDebugInfo("Woken up on initial call", metadata);
      env->ThrowError((std::string("Atomics.wait on thread ") +
          std::to_string(env->thread_id()) + " is unwakeable\n" +
          *debug_info).c_str());
    }
  }
}

std::shared_ptr<std::string> SharedArrayBufferMetadata::GenerateDebugInfo(
    const std::string& reason, SharedArrayBufferMetadataReference target) {
  std::string info = reason + "\n";
  std::unordered_set<SharedArrayBufferMetadataReference> sabs;
  if (target)
    sabs.insert(target);
  for (const auto& pair : waiting_isolates_) {
    sabs.insert(pair.second.sab);
    info += "Thread ";
    info += std::to_string(Worker::ThreadIdForIsolate(pair.first));
    info += " waits for SharedArrayBuffer ";
    info += std::to_string(pair.second.sab->debug_id_);
    info += "\n";
  }
  for (const SharedArrayBufferMetadataReference& sab : sabs) {
    for (Isolate* isolate : sab->accessing_isolates_) {
      info += "SharedArrayBuffer ";
      info += std::to_string(sab->debug_id_);
      info += " is accessible by thread ";
      info += std::to_string(Worker::ThreadIdForIsolate(isolate));
      info += "\n";
    }
    if (sab->in_transfer_count_ > 0) {
      info += "SharedArrayBuffer ";
      info += std::to_string(sab->debug_id_);
      info += " is waiting to be emitted from ";
      info += std::to_string(sab->in_transfer_count_);
      info += " messages\n";
    }
  }
  return std::make_shared<std::string>(std::move(info));
}

Mutex SharedArrayBufferMetadata::mutex_;

std::unordered_map<v8::Isolate*, SharedArrayBufferMetadata::wait_information>
    SharedArrayBufferMetadata::waiting_isolates_;

std::atomic_size_t SharedArrayBufferMetadata::next_debug_id_(0);

}  // namespace worker
}  // namespace node
