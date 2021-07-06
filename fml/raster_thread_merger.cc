// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#define FML_USED_ON_EMBEDDER

#include "flutter/fml/raster_thread_merger.h"

#include "flutter/fml/message_loop_impl.h"
#include <set>


namespace fml {

const int RasterThreadMerger::kLeaseNotSet = -1;

RasterThreadMerger::RasterThreadMerger(fml::TaskQueueId platform_queue_id,
                                       fml::TaskQueueId gpu_queue_id)
    : platform_queue_id_(platform_queue_id),
      gpu_queue_id_(gpu_queue_id),
      task_queues_(fml::MessageLoopTaskQueues::GetInstance()),
      lease_term_(kLeaseNotSet),
      enabled_(true) {}

void RasterThreadMerger::SetMergeUnmergeCallback(const fml::closure& callback) {
  merge_unmerge_callback_ = callback;
}

void RasterThreadMerger::MergeWithLease(size_t lease_term) {
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

  bool success = task_queues_->Merge(platform_queue_id_, gpu_queue_id_);
  if (success && merge_unmerge_callback_ != nullptr) {
    FML_LOG(ERROR) << "--- test ---- merge_unmerge_callback_()";
    merge_unmerge_callback_();
  }
  FML_CHECK(success) << "Unable to merge the raster and platform threads.";
  lease_term_ = lease_term;

  merged_condition_.notify_one();
}

void RasterThreadMerger::UnMergeByCaller(void* caller_embedder) {
  std::scoped_lock lock(lease_term_mutex_);
  size_t left_count = RemoveMergeCaller(caller_embedder);
  if (left_count > 0) {
    return; // TODO check sequence
  }
  if (TaskQueuesAreSame()) {
    return;
  }
  if (!IsEnabledUnSafe()) {
    return;
  }
  lease_term_ = 0;
  bool success = task_queues_->Unmerge(platform_queue_id_, gpu_queue_id_);
  if (success && merge_unmerge_callback_ != nullptr) {
    merge_unmerge_callback_();
  }
  FML_CHECK(success) << "Unable to un-merge the raster and platform threads.";
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
  return lease_term_ > 0 || TaskQueuesAreSame();
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

RasterThreadStatus RasterThreadMerger::DecrementLease(void* caller_embedder) {
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
  if (lease_term_ == 0) {
    // |UnMergeByCaller| is going to acquire the lock again.
    lock.unlock();
    UnMergeByCaller(caller_embedder);
    return RasterThreadStatus::kUnmergedNow;
  }

  return RasterThreadStatus::kRemainsMerged;
}

std::set<void *> merged_records;
std::mutex merged_records_mutex_;

size_t RasterThreadMerger::AddMergeCaller(void* caller) {
  std::scoped_lock scoped_lock(merged_records_mutex_);
  merged_records.insert(caller);
  size_t left_count = merged_records.size();
  return left_count;
}

size_t RasterThreadMerger::RemoveMergeCaller(void* caller) {
  std::scoped_lock scoped_lock(merged_records_mutex_);
  merged_records.erase(caller);
  size_t left_count = merged_records.size();
  return left_count;
}


}  // namespace fml
