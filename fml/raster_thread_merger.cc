// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#define FML_USED_ON_EMBEDDER

#include "flutter/fml/raster_thread_merger.h"

#include "flutter/fml/message_loop_impl.h"
#include <set>


namespace fml {

const int RealThreadMerger::kLeaseNotSet = -1;

RasterThreadMerger::RasterThreadMerger(fml::TaskQueueId platform_queue_id,
                                       fml::TaskQueueId gpu_queue_id)
    : platform_queue_id_(platform_queue_id),
      gpu_queue_id_(gpu_queue_id),
      task_queues_(fml::MessageLoopTaskQueues::GetInstance()),
      enabled_(true) {
  FML_LOG(ERROR) << "--- eggfly ---- RasterThreadMerger(), this=" << this
                 << ", task_queues_=" << task_queues_.get()
                 << ", platform_queue_id=" << platform_queue_id_
                 << ", gpu_queue_id_=" << gpu_queue_id_;
  // eggfly mod
  // FML_CHECK(!task_queues_->Owns(platform_queue_id_, gpu_queue_id_));
}

void RasterThreadMerger::SetMergeUnmergeCallback(const fml::closure& callback) {
  merge_unmerge_callback_ = callback;
}



void RasterThreadMerger::MergeWithLease(size_t lease_term) {
  FML_LOG(ERROR) << "--- eggfly ---- MergeWithLease(), this=" << this;

  std::scoped_lock lock(lease_term_mutex_);
  if (TaskQueuesAreSame()) {
    return;
  }
  if (!IsEnabledUnSafe()) {
    return;
  }
  FML_DCHECK(lease_term > 0) << "lease_term should be positive.";

  if (IsMergedUnSafe()) {
    merged_condition_.notify_one();
    return;
  }

  auto merger = RealThreadMerger::GetSharedRealMerger(platform_queue_id_, gpu_queue_id_);
  bool success = merger->MergeWithLease(this, lease_term);
  if (success && merge_unmerge_callback_ != nullptr) {
    merge_unmerge_callback_();
  }

  merged_condition_.notify_one();
}


fml::RefPtr<RealThreadMerger> RealThreadMerger::GetSharedRealMerger(fml::TaskQueueId owner, fml::TaskQueueId subsumed){
  // std::scoped_lock creation(cached_mergers_mutex_);
  ThreadMergerKey key = {.owner = owner, .subsumed = subsumed};
  if (real_mergers_.find(key) != real_mergers_.end()) {
    return real_mergers_[key];
  }
  auto merger = fml::MakeRefCounted<RealThreadMerger>(owner, subsumed);
  real_mergers_[key] = merger;
  FML_LOG(ERROR) << "---- eggfly ---- GetSharedRealMerger: miss " << owner << ", " << subsumed;
  return merger;
}

bool RealThreadMerger::MergeWithLease(RasterThreadMerger *caller, size_t lease_term) {
  if (!merged_callers_.empty()) {
    FML_LOG(ERROR) << "---- eggfly ---- MergeWithLease() already merged.. return";
    return true;
  }
  bool success = task_queues_->Merge(platform_queue_id_, gpu_queue_id_);
  FML_CHECK(success) << "Unable to merge the raster and platform threads.";

  merged_callers_.insert(caller);
  lease_term_ = lease_term;

  return success;
}

bool RealThreadMerger::UnmergeNow(RasterThreadMerger *caller) {
  FML_LOG(ERROR) << "--- eggfly ---- UnMergeNow(), this=" << this << ", size=" << merged_callers_.size();

  merged_callers_.erase(caller);
  if (!merged_callers_.empty()) {
    FML_LOG(ERROR) << "---- eggfly ---- UnmergeNow() need leave merged.. return";
    return true;
  }
  bool success = task_queues_->Unmerge(platform_queue_id_, gpu_queue_id_);
  FML_CHECK(success) << "Unable to un-merge the raster and platform threads.";
  lease_term_ = 0;
  return success;
}

void RasterThreadMerger::UnMergeNow() {
  std::scoped_lock lock(lease_term_mutex_);
  FML_LOG(ERROR) << "--- eggfly ---- UnMergeNow(), this=" << this;

  if (TaskQueuesAreSame()) {
    FML_LOG(ERROR) << "--- eggfly ---- TaskQueuesAreSame()";
    return;
  }
  if (!IsEnabledUnSafe()) {
    FML_LOG(ERROR) << "--- eggfly ---- !IsEnabledUnSafe()";
    return;
  }
  auto merger = RealThreadMerger::GetSharedRealMerger(platform_queue_id_, gpu_queue_id_);
  bool success = merger->UnmergeNow(this);
  if (success && merge_unmerge_callback_ != nullptr) {
    merge_unmerge_callback_();
  }
}

bool RasterThreadMerger::IsOnPlatformThread() const {
  return MessageLoop::GetCurrentTaskQueueId() == platform_queue_id_;
}

bool RasterThreadMerger::IsOnRasterizingThread() const {
  if (IsMergedUnSafe()) {
    return IsOnPlatformThread();
  } else {
    return !IsOnPlatformThread();
  }
}

void RasterThreadMerger::ExtendLeaseTo(size_t lease_term) {
  FML_DCHECK(lease_term > 0) << "lease_term should be positive.";
  FML_LOG(ERROR) << "--- eggfly ---- ExtendLeaseTo(), this=" << this << ", lease_term=" << lease_term;
  if (TaskQueuesAreSame()) {
    return;
  }
  std::scoped_lock lock(lease_term_mutex_);
  auto merger = RealThreadMerger::GetSharedRealMerger(platform_queue_id_, gpu_queue_id_);
  merger->ExtendLeaseTo(lease_term);
}

bool RasterThreadMerger::IsMerged() {
  std::scoped_lock lock(lease_term_mutex_);
  bool is_merged = IsMergedUnSafe();
  return is_merged;
}

void RasterThreadMerger::Enable() {
  std::scoped_lock lock(lease_term_mutex_);
  enabled_ = true;
}

void RasterThreadMerger::Disable() {
  std::scoped_lock lock(lease_term_mutex_);
  enabled_ = false;
}

bool RasterThreadMerger::IsEnabled() {
  std::scoped_lock lock(lease_term_mutex_);
  return IsEnabledUnSafe();
}

bool RasterThreadMerger::IsEnabledUnSafe() const {
  return enabled_;
}

bool RasterThreadMerger::IsMergedUnSafe() const {
//  bool is_merged = lease_term_ > 0 || TaskQueuesAreSame();
  bool is_merged = false;
  auto merger = RealThreadMerger::GetSharedRealMerger(platform_queue_id_, gpu_queue_id_);

  if (TaskQueuesAreSame()) {
    is_merged = true;
  } else if (merger->IsMergedUnSafe()) {
    is_merged = true;
  }
  FML_LOG(ERROR) << "--- eggfly ---- IsMergedUnSafe(), this=" << this << ", is_merged=" << is_merged;
  return is_merged;
}

bool RasterThreadMerger::TaskQueuesAreSame() const {
  return platform_queue_id_ == gpu_queue_id_;
}

void RasterThreadMerger::WaitUntilMerged() {
  if (TaskQueuesAreSame()) {
    return;
  }
  FML_CHECK(IsOnPlatformThread());
  std::unique_lock<std::mutex> lock(lease_term_mutex_);
  merged_condition_.wait(lock, [&] { return IsMergedUnSafe(); });
}

RasterThreadStatus RasterThreadMerger::DecrementLease() {
  if (TaskQueuesAreSame()) {
    return RasterThreadStatus::kRemainsMerged;
  }
  std::unique_lock<std::mutex> lock(lease_term_mutex_);
  if (!IsMergedUnSafe()) {
    return RasterThreadStatus::kRemainsUnmerged;
  }
  if (!IsEnabledUnSafe()) {
    return RasterThreadStatus::kRemainsMerged;
  }
  auto merger = RealThreadMerger::GetSharedRealMerger(platform_queue_id_, gpu_queue_id_);
  bool need_unmerge_now = merger->DecrementLease();

  if (need_unmerge_now) {
    // |UnMergeNow| is going to acquire the lock again.
    lock.unlock();
    UnMergeNow();
    return RasterThreadStatus::kUnmergedNow;
  }

  return RasterThreadStatus::kRemainsMerged;
}

bool RealThreadMerger::DecrementLease() {
  FML_DCHECK(lease_term_ > 0)
  << "lease_term should always be positive when merged.";
  lease_term_--;
  FML_LOG(ERROR) << "--- eggfly ---- DecrementLease(), this=" << this << ", lease_term_--=" << lease_term_;
  return lease_term_ == 0;
}

RealThreadMerger::RealThreadMerger(fml::TaskQueueId platform_queue_id, fml::TaskQueueId gpu_queue_id)
    : platform_queue_id_(platform_queue_id),
      gpu_queue_id_(gpu_queue_id),
      task_queues_(fml::MessageLoopTaskQueues::GetInstance()),
      lease_term_(kLeaseNotSet) {}

void RealThreadMerger::ExtendLeaseTo(size_t lease_term) {
  FML_DCHECK(IsMergedUnSafe()) << "should be merged state when calling this method";
  if (lease_term_ != kLeaseNotSet &&
      static_cast<int>(lease_term) > lease_term_) {
    lease_term_ = lease_term;
  }
}

bool RealThreadMerger::IsMergedUnSafe() const {
  // TODO
  return !merged_callers_.empty();
}

std::map<ThreadMergerKey, fml::RefPtr<RealThreadMerger>> RealThreadMerger::real_mergers_;

}  // namespace fml
