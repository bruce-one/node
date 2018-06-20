#ifndef SRC_SHAREDARRAYBUFFER_METADATA_H_
#define SRC_SHAREDARRAYBUFFER_METADATA_H_

#if defined(NODE_WANT_INTERNALS) && NODE_WANT_INTERNALS

#include "node.h"
#include "node_mutex.h"
#include <atomic>
#include <memory>
#include <unordered_map>
#include <unordered_set>

namespace node {
namespace worker {

class SharedArrayBufferMetadata;
class SABLifetimePartner;

// This is an object associated with a SharedArrayBuffer, which keeps track
// of a cross-thread reference count. Once a SharedArrayBuffer is transferred
// for the first time (or is attempted to be transferred), one of these objects
// is created, and the SharedArrayBuffer is moved from internalized mode into
// externalized mode (i.e. the JS engine no longer frees the memory on its own).
//
// This will always be referred to using a std::shared_ptr, since it keeps
// a reference count and is guaranteed to be thread-safe.
typedef std::shared_ptr<SharedArrayBufferMetadata>
    SharedArrayBufferMetadataReference;

class SharedArrayBufferMetadata
    : public std::enable_shared_from_this<SharedArrayBufferMetadata> {
 public:
  static SharedArrayBufferMetadataReference ForSharedArrayBuffer(
      Environment* env,
      v8::Local<v8::Context> context,
      v8::Local<v8::SharedArrayBuffer> source,
      bool may_attach_new_reference = true);
  ~SharedArrayBufferMetadata();

  // Create a SharedArrayBuffer object for a specific Environment and Context.
  // The created SharedArrayBuffer will be in externalized mode and has
  // a hidden object attached to it, during whose lifetime the reference
  // count is increased by 1.
  v8::MaybeLocal<v8::SharedArrayBuffer> GetSharedArrayBuffer(
      Environment* env, v8::Local<v8::Context> context);

  SharedArrayBufferMetadata(SharedArrayBufferMetadata&& other) = delete;
  SharedArrayBufferMetadata& operator=(
      SharedArrayBufferMetadata&& other) = delete;
  SharedArrayBufferMetadata& operator=(
      const SharedArrayBufferMetadata&) = delete;
  SharedArrayBufferMetadata(const SharedArrayBufferMetadata&) = delete;

  static void AtomicsWaitCallback(
      v8::Isolate::AtomicsWaitEvent event,
      v8::Local<v8::SharedArrayBuffer> array_buffer,
      size_t offset_in_bytes,
      int32_t value,
      double timeout_in_ms,
      v8::Isolate::AtomicsWaitWakeHandle* wake_handle,
      void* data);

  void IncreaseInTransferCount();
  void DecreaseInTransferCount();

 private:
  friend class SABLifetimePartner;

  explicit SharedArrayBufferMetadata(void* data, size_t size);

  // Attach a lifetime tracker object with a reference count to `target`.
  v8::Maybe<bool> AssignToSharedArrayBuffer(
      Environment* env,
      v8::Local<v8::Context> context,
      v8::Local<v8::SharedArrayBuffer> target);

  void* data = nullptr;
  size_t size = 0;

  static std::atomic_size_t next_debug_id_;
  size_t debug_id_;

  static bool CanBeWokenUp(
      v8::Isolate* isolate,
      std::unordered_set<v8::Isolate*>* already_visited = nullptr);
  void CheckAllWaitersForDeadlock(const std::string& reason);

  struct wait_information {
    SharedArrayBufferMetadataReference sab;
    v8::Isolate::AtomicsWaitWakeHandle* wake_handle;
    std::shared_ptr<std::string> debug_info = std::shared_ptr<std::string>();
  };

  static std::shared_ptr<std::string> GenerateDebugInfo(
      const std::string& reason,
      SharedArrayBufferMetadataReference target = nullptr);

  static Mutex mutex_;
  size_t in_transfer_count_ = 0;
  std::unordered_multiset<v8::Isolate*> accessing_isolates_;
  static std::unordered_map<v8::Isolate*, wait_information> waiting_isolates_;
};

}  // namespace worker
}  // namespace node

#endif  // defined(NODE_WANT_INTERNALS) && NODE_WANT_INTERNALS


#endif  // SRC_SHAREDARRAYBUFFER_METADATA_H_
