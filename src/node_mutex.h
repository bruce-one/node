#ifndef SRC_NODE_MUTEX_H_
#define SRC_NODE_MUTEX_H_

#include "util.h"
#include "uv.h"

namespace node {

template <typename Traits> class ConditionVariableBase;
template <typename Traits> class MutexBase;
struct LibuvMutexTraits;

using ConditionVariable = ConditionVariableBase<LibuvMutexTraits>;
using Mutex = MutexBase<LibuvMutexTraits>;

template <typename Traits>
class MutexBase {
 public:
  inline MutexBase();
  inline ~MutexBase();
  inline void Lock();
  inline void Unlock();

  class ScopedLock;
  class ScopedUnlock;

  class ScopedLock {
   public:
    inline explicit ScopedLock(MutexBase* mutex);
    inline explicit ScopedLock(ScopedUnlock* scoped_unlock);
    inline ~ScopedLock();

   private:
    template <typename> friend class ConditionVariableBase;
    friend class ScopedUnlock;
    MutexBase* const mutex_;
    DISALLOW_COPY_AND_ASSIGN(ScopedLock);
  };

  class ScopedUnlock {
   public:
    inline explicit ScopedUnlock(ScopedLock* scoped_lock);
    inline ~ScopedUnlock();

   private:
    friend class ScopedLock;
    MutexBase* const mutex_;
    DISALLOW_COPY_AND_ASSIGN(ScopedUnlock);
  };

 private:
  template <typename> friend class ConditionVariableBase;
  typename Traits::MutexT mutex_;
  DISALLOW_COPY_AND_ASSIGN(MutexBase);
};

template <typename Traits>
class ConditionVariableBase {
 public:
  using ScopedLock = typename MutexBase<Traits>::ScopedLock;

  inline ConditionVariableBase();
  inline ~ConditionVariableBase();
  inline void Broadcast(const ScopedLock&);
  inline void Signal(const ScopedLock&);
  inline void Wait(const ScopedLock& scoped_lock);

 private:
  typename Traits::CondT cond_;
  DISALLOW_COPY_AND_ASSIGN(ConditionVariableBase);
};

struct LibuvMutexTraits {
  using CondT = uv_cond_t;
  using MutexT = uv_mutex_t;
  static constexpr int (*cond_init)(CondT*) = uv_cond_init;
  static constexpr int (*mutex_init)(MutexT*) = uv_mutex_init;
  static constexpr void (*cond_broadcast)(CondT*) = uv_cond_broadcast;
  static constexpr void (*cond_destroy)(CondT*) = uv_cond_destroy;
  static constexpr void (*cond_signal)(CondT*) = uv_cond_signal;
  static constexpr void (*cond_wait)(CondT*, MutexT*) = uv_cond_wait;
  static constexpr void (*mutex_destroy)(MutexT*) = uv_mutex_destroy;
  static constexpr void (*mutex_lock)(MutexT*) = uv_mutex_lock;
  static constexpr void (*mutex_unlock)(MutexT*) = uv_mutex_unlock;
};

template <typename Traits>
ConditionVariableBase<Traits>::ConditionVariableBase() {
  CHECK_EQ(0, Traits::cond_init(&cond_));
}

template <typename Traits>
ConditionVariableBase<Traits>::~ConditionVariableBase() {
  Traits::cond_destroy(&cond_);
}

template <typename Traits>
void ConditionVariableBase<Traits>::Broadcast(const ScopedLock&) {
  Traits::cond_broadcast(&cond_);
}

template <typename Traits>
void ConditionVariableBase<Traits>::Signal(const ScopedLock&) {
  Traits::cond_signal(&cond_);
}

template <typename Traits>
void ConditionVariableBase<Traits>::Wait(const ScopedLock& scoped_lock) {
  Traits::cond_wait(&cond_, &scoped_lock.mutex_->mutex_);
}

template <typename Traits>
MutexBase<Traits>::MutexBase() {
  CHECK_EQ(0, Traits::mutex_init(&mutex_));
}

template <typename Traits>
MutexBase<Traits>::~MutexBase() {
  Traits::mutex_destroy(&mutex_);
}

template <typename Traits>
void MutexBase<Traits>::Lock() {
  Traits::mutex_lock(&mutex_);
}

template <typename Traits>
void MutexBase<Traits>::Unlock() {
  Traits::mutex_unlock(&mutex_);
}

template <typename Traits>
MutexBase<Traits>::ScopedLock::ScopedLock(MutexBase* mutex) : mutex_(mutex) {
  mutex_->Lock();
}

template <typename Traits>
MutexBase<Traits>::ScopedLock::ScopedLock(ScopedUnlock* scoped_unlock)
    : mutex_(scoped_unlock->mutex_) {
  mutex_->Lock();
}

template <typename Traits>
MutexBase<Traits>::ScopedLock::~ScopedLock() {
  mutex_->Unlock();
}

template <typename Traits>
MutexBase<Traits>::ScopedUnlock::ScopedUnlock(ScopedLock* scoped_lock)
    : mutex_(scoped_lock->mutex_) {
  mutex_->Unlock();
}

template <typename Traits>
MutexBase<Traits>::ScopedUnlock::~ScopedUnlock() {
  mutex_->Lock();
}

}  // namespace node

#endif  // SRC_NODE_MUTEX_H_
