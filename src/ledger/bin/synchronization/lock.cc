// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/ledger/bin/synchronization/lock.h"

#include <lib/fit/function.h>

#include "src/ledger/lib/memory/weak_ptr.h"

namespace lock {
namespace {
class LockImpl : public Lock {
 public:
  LockImpl() : weak_ptr_factory_(this) {}
  LockImpl(const LockImpl&) = delete;
  LockImpl& operator=(const LockImpl&) = delete;

  ~LockImpl() override {
    if (serialization_callback_) {
      serialization_callback_();
    }
  }

  coroutine::ContinuationStatus Acquire(coroutine::CoroutineHandler* const handler,
                                        ledger::OperationSerializer* const serializer) {
    return SyncCall(handler, [weak_this = weak_ptr_factory_.GetWeakPtr(),
                              serializer](fit::function<void()> sync_callback) {
      serializer->Serialize<>([] {},
                              [weak_this, sync_callback = std::move(sync_callback)](
                                  fit::closure serialization_callback) mutable {
                                // Moving sync_callback to the stack as the serialization_callback
                                // might delete this closure.
                                auto sync_callback_local = std::move(sync_callback);
                                if (weak_this) {
                                  weak_this->serialization_callback_ =
                                      std::move(serialization_callback);
                                } else {
                                  serialization_callback();
                                }
                                sync_callback_local();
                              });
    });
  }

 private:
  fit::closure serialization_callback_;

  ledger::WeakPtrFactory<LockImpl> weak_ptr_factory_;
};
}  // namespace

coroutine::ContinuationStatus AcquireLock(coroutine::CoroutineHandler* const handler,
                                          ledger::OperationSerializer* const serializer,
                                          std::unique_ptr<Lock>* lock) {
  std::unique_ptr<LockImpl> impl = std::make_unique<LockImpl>();
  coroutine::ContinuationStatus status = impl->Acquire(handler, serializer);
  if (status == coroutine::ContinuationStatus::OK) {
    *lock = std::move(impl);
  }
  return status;
}

}  // namespace lock
