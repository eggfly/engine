// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/fml/memory/task_runner_checker.h"

namespace fml {

TaskRunnerChecker::TaskRunnerChecker()
    : initialized_queue_id_(InitTaskQueueId()),
      subsumed_queue_id_(
          MessageLoopTaskQueues::GetInstance()->GetSubsumedTaskQueueId(
              initialized_queue_id_)){};

TaskRunnerChecker::~TaskRunnerChecker() = default;

/// TODO eggfly
bool TaskRunnerChecker::RunsOnCreationTaskRunner() const {
  FML_CHECK(fml::MessageLoop::IsInitializedForCurrentThread());
  const auto current_queue_id = MessageLoop::GetCurrentTaskQueueId();
  bool asSameAsInitial = RunsOnTheSameThread(current_queue_id, initialized_queue_id_);
  bool asSameAsSubsumed =
      (std::find(subsumed_queue_id_.begin(), subsumed_queue_id_.end(), current_queue_id) != subsumed_queue_id_.end());
//  FML_LOG(ERROR)
//  << "---- test ---- curr=" << current_queue_id << ", init=" << initialized_queue_id_ << ", sub.size="
//  << subsumed_queue_id_.size()
//  << ", asSameAsInitial=" << asSameAsInitial << ", asSameAsSubsumed=" << asSameAsSubsumed;
  return asSameAsInitial || asSameAsSubsumed;
};

bool TaskRunnerChecker::RunsOnTheSameThread(TaskQueueId queue_a,
                                            TaskQueueId queue_b) {
  if (queue_a == queue_b) {
    return true;
  }

  auto queues = MessageLoopTaskQueues::GetInstance();
  if (queues->Owns(queue_a, queue_b)) {
    return true;
  }
  if (queues->Owns(queue_b, queue_a)) {
    return true;
  }
  return false;
};

TaskQueueId TaskRunnerChecker::InitTaskQueueId() {
  MessageLoop::EnsureInitializedForCurrentThread();
  return MessageLoop::GetCurrentTaskQueueId();
};

}  // namespace fml
