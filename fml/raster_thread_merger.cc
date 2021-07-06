// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#define FML_USED_ON_EMBEDDER

#include "flutter/fml/raster_thread_merger.h"

#include "flutter/fml/message_loop_impl.h"
#include <set>

namespace fml {

const int RasterThreadMerger::kLeaseNotSet = -1;

static std::map<ThreadMergerKey, fml::RefPtr<RealThreadMerger>> mergers;
static std::mutex cached_mergers_mutex_;

RealThreadMerger::RealThreadMerger(fml::TaskQueueId owner, fml::TaskQueueId subsumed)
    : task_queues_(fml::MessageLoopTaskQueues::GetInstance()), owner_(owner), subsumed_(subsumed) {}

bool RealThreadMerger::Merge(RasterThreadMerger *caller) {
  FML_LOG(ERROR)
  << "---- eggfly ---- RealThreadMerger::Merge() before lock, caller=" << caller << ", owner=" << owner_
  << ", subsumed=" << subsumed_;
  std::unique_lock lock(mutex_);
  FML_LOG(ERROR)
  << "---- eggfly ---- RealThreadMerger::Merge(), after lock, caller=" << caller << ", owner=" << owner_
  << ", subsumed=" << subsumed_;
  lock.unlock();
  bool is_merged = IsMerged();
  AddMergeCaller(caller);
  if (is_merged) {
    return true;
  }
  bool success = task_queues_->Merge(owner_, subsumed_);
  FML_CHECK(success) << "Unable to merge the raster and platform threads.";
  return success;
}

bool RealThreadMerger::UnMerge(RasterThreadMerger *caller) {
  std::unique_lock lock(mutex_);
  FML_LOG(ERROR) << "---- eggfly ---- RealThreadMerger::UnMerge(), caller=" << caller;
  lock.unlock();
  size_t left_count = RemoveMergeCaller(caller);
  if (left_count > 0) {
    FML_LOG(ERROR) << "---- eggfly ---- RealThreadMerger::UnMerge(), left_count > 0 return. left_count=" << left_count;
    return true; // TODO false? true? check sequence
  }

  bool success = task_queues_->Unmerge(owner_, subsumed_);
  FML_CHECK(success) << "Unable to un-merge the raster and platform threads.";
  return success;
}

fml::RefPtr<RealThreadMerger> fml::RealThreadMerger::GetCachedThreadMerger(TaskQueueId owner, TaskQueueId subsumed) {
  std::scoped_lock creation(cached_mergers_mutex_);
  ThreadMergerKey key = {.owner = owner, .subsumed = subsumed};
  if (mergers.find(key) != mergers.end()) {
    return mergers[key];
  }
  auto merger = fml::MakeRefCounted<RealThreadMerger>(owner, subsumed);
  mergers[key] = merger;
  FML_LOG(ERROR) << "---- eggfly ---- GetCachedThreadMerger: miss " << owner << ", " << subsumed;
  return merger;
}

RasterThreadMerger::RasterThreadMerger(fml::TaskQueueId platform_queue_id,
                                       fml::TaskQueueId gpu_queue_id)
    : platform_queue_id_(platform_queue_id),
      gpu_queue_id_(gpu_queue_id),
      task_queues_(fml::MessageLoopTaskQueues::GetInstance()),
      lease_term_(kLeaseNotSet),
      enabled_(true) {}

RasterThreadMerger::~RasterThreadMerger() {
  FML_LOG(ERROR)
  << "---- eggfly ---- ~RasterThreadMerger(), current=" << fml::MessageLoop::GetCurrentTaskQueueId();
}

void RasterThreadMerger::SetMergeUnmergeCallback(const fml::closure& callback) {
  merge_unmerge_callback_ = callback;
}

void RasterThreadMerger::MergeWithLease(size_t lease_term) {
  FML_LOG(ERROR)
  << "---- eggfly ---- MergeWithLease(), " << platform_queue_id_ << ", " << gpu_queue_id_ << ", records.size="
  << mergers.size();
  std::scoped_lock lock(lease_term_mutex_);
  if (TaskQueuesAreSame()) {
    return;
  }
  if (!IsEnabledUnSafe()) {
    return;
  }
  FML_DCHECK(lease_term > 0) << "lease_term should be positive.";

  if (IsMergedUnSafe()) {
    // merged_condition_.notify_one();
    return;
  }

  auto common_merger = RealThreadMerger::GetCachedThreadMerger(platform_queue_id_, gpu_queue_id_);

  bool success = common_merger->Merge(this);

  if (success && merge_unmerge_callback_ != nullptr) {
    FML_LOG(ERROR) << "---- eggfly ---- merge_unmerge_callback_()";
    merge_unmerge_callback_();
  }
  FML_CHECK(success) << "Unable to merge the raster and platform threads.";
  lease_term_ = lease_term;

  // merged_condition_.notify_one();
}

void RasterThreadMerger::UnMergeNow() {
  std::scoped_lock lock(lease_term_mutex_);
  FML_LOG(ERROR)
  << "---- eggfly ---- RasterThreadMerger::UnMergeNow(), " << platform_queue_id_ << ", " << gpu_queue_id_;
  if (TaskQueuesAreSame()) {
    FML_LOG(ERROR) << "---- eggfly ---- UnMergeNow(), TaskQueuesAreSame() return.";
    return;
  }
  if (!IsEnabledUnSafe()) {
    FML_LOG(ERROR) << "---- eggfly ---- UnMergeNow(), !IsEnabledUnSafe() return.";
    return;
  }

  lease_term_ = 0; // TODO
  auto common_merger = RealThreadMerger::GetCachedThreadMerger(platform_queue_id_, gpu_queue_id_);
  bool success = common_merger->UnMerge(this);
  FML_LOG(ERROR) << "---- eggfly ---- UnMergeNow(), result=" << success;
  if (success && merge_unmerge_callback_ != nullptr) {
    FML_LOG(ERROR) << "---- eggfly ---- merge_unmerge_callback_()";
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
  FML_LOG(ERROR) << "ExtendLeaseTo() " << lease_term;
  if (TaskQueuesAreSame()) {
    return;
  }
  std::scoped_lock lock(lease_term_mutex_);
  FML_DCHECK(IsMergedUnSafe()) << "lease_term should be positive.";
  if (lease_term_ != kLeaseNotSet &&
      static_cast<int>(lease_term) > lease_term_) {
    lease_term_ = lease_term;
  }
}

bool RasterThreadMerger::IsMerged() {
  std::scoped_lock lock(lease_term_mutex_);
  return IsMergedUnSafe();
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
  return TaskQueuesAreSame() || RealThreadMerger::GetCachedThreadMerger(platform_queue_id_, gpu_queue_id_)->IsMerged();
}

bool RasterThreadMerger::TaskQueuesAreSame() const {
  return platform_queue_id_ == gpu_queue_id_;
}
//
//void RasterThreadMerger::WaitUntilMerged() {
//  if (TaskQueuesAreSame()) {
//    return;
//  }
//  FML_CHECK(IsOnPlatformThread());
//  std::unique_lock<std::mutex> lock(lease_term_mutex_);
//  merged_condition_.wait(lock, [&] { return IsMergedUnSafe(); });
//}

RasterThreadStatus RasterThreadMerger::DecrementLease() {
  FML_LOG(ERROR) << "---- eggfly ---- DecrementLease(), lease=" << lease_term_;
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
  FML_DCHECK(lease_term_ > 0)
      << "lease_term should always be positive when merged.";
  lease_term_--;
  FML_LOG(ERROR) << "---- eggfly ---- DecrementLease(), lease--=" << lease_term_;
  if (lease_term_ == 0) {
    // |UnMergeNow| is going to acquire the lock again.
    lock.unlock();
    UnMergeNow();
    return RasterThreadStatus::kUnmergedNow;
  }

  return RasterThreadStatus::kRemainsMerged;
}

size_t RealThreadMerger::AddMergeCaller(RasterThreadMerger* caller) {
  std::scoped_lock lock(mutex_);
  FML_LOG(ERROR) << "---- eggfly ---- AddMergeCaller: caller=" << caller;
  merger_callers_.insert(caller);
  size_t left_count = merger_callers_.size();
  return left_count;
}

bool RealThreadMerger::IsMerged() {
  std::scoped_lock lock(mutex_);
  return !merger_callers_.empty();
}

size_t RealThreadMerger::RemoveMergeCaller(RasterThreadMerger* caller) {
  std::scoped_lock lock(mutex_);
  merger_callers_.erase(caller);
  size_t left_count = merger_callers_.size();
  return left_count;
}


}  // namespace fml
