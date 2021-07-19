// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#define FML_USED_ON_EMBEDDER

#include "flutter/fml/message_loop_task_queues.h"

#include <iostream>
#include <list>
#include <memory>

#include "flutter/fml/make_copyable.h"
#include "flutter/fml/message_loop_impl.h"
#include "flutter/fml/task_source.h"
#include "flutter/fml/thread_local.h"

namespace fml {

std::mutex MessageLoopTaskQueues::creation_mutex_;

const size_t TaskQueueId::kUnmerged = ULONG_MAX;

// Guarded by creation_mutex_.
fml::RefPtr<MessageLoopTaskQueues> MessageLoopTaskQueues::instance_;


namespace {

// iOS prior to version 9 prevents c++11 thread_local and __thread specefier,
// having us resort to boxed enum containers.
class TaskSourceGradeHolder {
 public:
  TaskSourceGrade task_source_grade;

  explicit TaskSourceGradeHolder(TaskSourceGrade task_source_grade_arg)
      : task_source_grade(task_source_grade_arg) {}
};
}  // namespace

// Guarded by creation_mutex_.
FML_THREAD_LOCAL ThreadLocalUniquePtr<TaskSourceGradeHolder>
    tls_task_source_grade;

TaskQueueEntry::TaskQueueEntry(TaskQueueId created_for_arg)
    : subsumed_by(_kUnmerged),
      created_for(created_for_arg) {
  wakeable = NULL;
  task_observers = TaskObservers();
  task_source = std::make_unique<TaskSource>(created_for);
}

fml::RefPtr<MessageLoopTaskQueues> MessageLoopTaskQueues::GetInstance() {
  std::scoped_lock creation(creation_mutex_);
  if (!instance_) {
    instance_ = fml::MakeRefCounted<MessageLoopTaskQueues>();
    tls_task_source_grade.reset(
        new TaskSourceGradeHolder{TaskSourceGrade::kUnspecified});
  }
  return instance_;
}


TaskQueueId MessageLoopTaskQueues::CreateTaskQueue() {
  std::lock_guard guard(queue_mutex_);
  TaskQueueId loop_id = TaskQueueId(task_queue_id_counter_);
  ++task_queue_id_counter_;
  queue_entries_[loop_id] = std::make_unique<TaskQueueEntry>(loop_id);
  return loop_id;
}

MessageLoopTaskQueues::MessageLoopTaskQueues()
    : task_queue_id_counter_(0), order_(0) {}

MessageLoopTaskQueues::~MessageLoopTaskQueues() = default;

void MessageLoopTaskQueues::Dispose(TaskQueueId queue_id) {
  std::lock_guard guard(queue_mutex_);
  const auto& queue_entry = queue_entries_.at(queue_id);
  FML_DCHECK(queue_entry->subsumed_by == _kUnmerged);
  auto &subsumed_set = queue_entry->owner_of;
  queue_entries_.erase(queue_id);
  for (TaskQueueId subsumed :subsumed_set) {
    if (subsumed != _kUnmerged) {
      queue_entries_.erase(subsumed);
    }
  }
}

void MessageLoopTaskQueues::DisposeTasks(TaskQueueId queue_id) {
  std::lock_guard guard(queue_mutex_);
  const auto& queue_entry = queue_entries_.at(queue_id);
  FML_DCHECK(queue_entry->subsumed_by == _kUnmerged);
  queue_entry->task_source->ShutDown();
  for (TaskQueueId subsumed: queue_entry->owner_of) {
    if (subsumed != _kUnmerged) {
      queue_entries_.at(subsumed)->task_source->ShutDown();
    }
  }
}

TaskSourceGrade MessageLoopTaskQueues::GetCurrentTaskSourceGrade() {
  std::scoped_lock creation(creation_mutex_);
  return tls_task_source_grade.get()->task_source_grade;
}

void MessageLoopTaskQueues::RegisterTask(
    TaskQueueId queue_id,
    const fml::closure& task,
    fml::TimePoint target_time,
    fml::TaskSourceGrade task_source_grade) {
  std::lock_guard guard(queue_mutex_);
  size_t order = order_++;
  const auto& queue_entry = queue_entries_.at(queue_id);
  queue_entry->task_source->RegisterTask(
      {order, task, target_time, task_source_grade});
  TaskQueueId loop_to_wake = queue_id;
  if (queue_entry->subsumed_by != _kUnmerged) {
    loop_to_wake = queue_entry->subsumed_by;
  }

  // This can happen when the secondary tasks are paused.
  if (HasPendingTasksUnlocked(loop_to_wake)) {
    WakeUpUnlocked(loop_to_wake, GetNextWakeTimeUnlocked(loop_to_wake));
  }
}

bool MessageLoopTaskQueues::HasPendingTasks(TaskQueueId queue_id) const {
  std::lock_guard guard(queue_mutex_);
  return HasPendingTasksUnlocked(queue_id);
}

fml::closure MessageLoopTaskQueues::GetNextTaskToRun(TaskQueueId queue_id,
                                                     fml::TimePoint from_time) {
  std::lock_guard guard(queue_mutex_);
  if (!HasPendingTasksUnlocked(queue_id)) {
    return nullptr;
  }
  TaskSource::TopTask top = PeekNextTaskUnlocked(queue_id);

  if (!HasPendingTasksUnlocked(queue_id)) {
    WakeUpUnlocked(queue_id, fml::TimePoint::Max());
  } else {
    WakeUpUnlocked(queue_id, GetNextWakeTimeUnlocked(queue_id));
  }

  if (top.task.GetTargetTime() > from_time) {
    return nullptr;
  }
  fml::closure invocation = top.task.GetTask();
  queue_entries_.at(top.task_queue_id)
      ->task_source->PopTask(top.task.GetTaskSourceGrade());
  {
    std::scoped_lock creation(creation_mutex_);
    const auto task_source_grade = top.task.GetTaskSourceGrade();
    tls_task_source_grade.reset(new TaskSourceGradeHolder{task_source_grade});
  }
  return invocation;
}

void MessageLoopTaskQueues::WakeUpUnlocked(TaskQueueId queue_id,
                                           fml::TimePoint time) const {
  if (queue_entries_.at(queue_id)->wakeable) {
    queue_entries_.at(queue_id)->wakeable->WakeUp(time);
  }
}

size_t MessageLoopTaskQueues::GetNumPendingTasks(TaskQueueId queue_id) const {
  std::lock_guard guard(queue_mutex_);
  const auto& queue_entry = queue_entries_.at(queue_id);
  if (queue_entry->subsumed_by != _kUnmerged) {
    return 0;
  }

  size_t total_tasks = 0;
  total_tasks += queue_entry->task_source->GetNumPendingTasks();

  for (TaskQueueId subsumed: queue_entry->owner_of) {
    if (subsumed != _kUnmerged) {
      const auto &subsumed_entry = queue_entries_.at(subsumed);
      total_tasks += subsumed_entry->task_source->GetNumPendingTasks();
    }
  }
  return total_tasks;
}

void MessageLoopTaskQueues::AddTaskObserver(TaskQueueId queue_id,
                                            intptr_t key,
                                            const fml::closure& callback) {
  std::lock_guard guard(queue_mutex_);
  FML_DCHECK(callback != nullptr) << "Observer callback must be non-null.";
  queue_entries_.at(queue_id)->task_observers[key] = callback;
}

void MessageLoopTaskQueues::RemoveTaskObserver(TaskQueueId queue_id,
                                               intptr_t key) {
  std::lock_guard guard(queue_mutex_);
  queue_entries_.at(queue_id)->task_observers.erase(key);
}

std::vector<fml::closure> MessageLoopTaskQueues::GetObserversToNotify(
    TaskQueueId queue_id) const {
  std::lock_guard guard(queue_mutex_);
  std::vector<fml::closure> observers;

  if (queue_entries_.at(queue_id)->subsumed_by != _kUnmerged) {
    return observers;
  }

  for (const auto& observer : queue_entries_.at(queue_id)->task_observers) {
    observers.push_back(observer.second);
  }

  for (TaskQueueId subsumed: queue_entries_.at(queue_id)->owner_of) {
    if (subsumed != _kUnmerged) {
      for (const auto& observer : queue_entries_.at(subsumed)->task_observers) {
        observers.push_back(observer.second);
      }
    }
  }

  return observers;
}

void MessageLoopTaskQueues::SetWakeable(TaskQueueId queue_id,
                                        fml::Wakeable* wakeable) {
  std::lock_guard guard(queue_mutex_);
  FML_CHECK(!queue_entries_.at(queue_id)->wakeable)
  << "Wakeable can only be set once.";
  queue_entries_.at(queue_id)->wakeable = wakeable;
}

/// TODO: always return true?
bool MessageLoopTaskQueues::Merge(TaskQueueId owner, TaskQueueId subsumed) {
  if (owner == subsumed) {
    return true;
  }
  std::lock_guard guard(queue_mutex_);
  auto& owner_entry = queue_entries_.at(owner);
  auto& subsumed_entry = queue_entries_.at(subsumed);

  if (OwnsUnlocked(owner, subsumed)) {
    return true;
  }

  // owner_entry->owner_of may contains items, so don't check owner_entry->owner_of
  // check owner_entry wasn't subsumed by others
  FML_DCHECK(owner_entry->subsumed_by == _kUnmerged);
  // check subsumed_entry didn't owns others
  FML_CHECK(subsumed_entry->owner_of.empty());
  // check subsumed_entry wasn't subsumed by others
  FML_CHECK(subsumed_entry->subsumed_by == _kUnmerged);

  owner_entry->owner_of.insert(subsumed);
  subsumed_entry->subsumed_by = owner;

  if (HasPendingTasksUnlocked(owner)) {
    WakeUpUnlocked(owner, GetNextWakeTimeUnlocked(owner));
  }

  return true;
}

/// always true?
bool MessageLoopTaskQueues::Unmerge(TaskQueueId owner, TaskQueueId subsumed) {
  FML_CHECK(owner != _kUnmerged);
  FML_CHECK(subsumed != _kUnmerged);
  std::lock_guard guard(queue_mutex_);
  const auto& owner_entry = queue_entries_.at(owner);
  // auto& subsumed_set = owner_entry->owner_of;

  queue_entries_.at(subsumed)->subsumed_by = _kUnmerged;
  owner_entry->owner_of.erase(subsumed);

  if (HasPendingTasksUnlocked(owner)) {
    WakeUpUnlocked(owner, GetNextWakeTimeUnlocked(owner));
  }

  if (HasPendingTasksUnlocked(subsumed)) {
    WakeUpUnlocked(subsumed, GetNextWakeTimeUnlocked(subsumed));
  }

  return true;
}

bool MessageLoopTaskQueues::Owns(TaskQueueId owner,
                                 TaskQueueId subsumed) const {
  std::lock_guard guard(queue_mutex_);
  return OwnsUnlocked(owner, subsumed);
}


std::map<TaskQueueId, int> counter;
size_t last_owner;
std::mutex counter_mutex;

bool MessageLoopTaskQueues::OwnsUnlocked(TaskQueueId owner,
                                         TaskQueueId subsumed) const {
//  if (owner == last_owner) {
//    if (counter.count(owner) == 0) {
//      counter[owner] = 0;
//    }
//    counter[owner]++;
//  }
//  last_owner = owner;
//  if (counter[owner] > 10000 && owner != 1) {
//    // FML_LOG(ERROR) << "---- eggfly ---- abort(): owner=" << owner << ", count=" << counter[owner];
//    // abort();
//  }
  // FML_LOG(ERROR) << "---- eggfly ---- owner=" << owner << ", subsumed=" << subsumed;
  auto &subsumed_set = queue_entries_.at(owner)->owner_of;
  bool owns = owner != _kUnmerged && subsumed != _kUnmerged &&
      (std::find(subsumed_set.begin(), subsumed_set.end(), subsumed) != subsumed_set.end());
  return owns;
}

const std::set<TaskQueueId> MessageLoopTaskQueues::GetSubsumedTaskQueueId(
    TaskQueueId owner) const {
  std::lock_guard guard(queue_mutex_);
  return queue_entries_.at(owner)->owner_of; // TODO TODO TODO
}

void MessageLoopTaskQueues::PauseSecondarySource(TaskQueueId queue_id) {
  std::lock_guard guard(queue_mutex_);
  queue_entries_.at(queue_id)->task_source->PauseSecondary();
}

void MessageLoopTaskQueues::ResumeSecondarySource(TaskQueueId queue_id) {
  std::lock_guard guard(queue_mutex_);
  queue_entries_.at(queue_id)->task_source->ResumeSecondary();
  // Schedule a wake as needed.
  if (HasPendingTasksUnlocked(queue_id)) {
    WakeUpUnlocked(queue_id, GetNextWakeTimeUnlocked(queue_id));
  }
}

// Subsumed queues will never have pending tasks.
// Owning queues will consider both their and their subsumed tasks.
bool MessageLoopTaskQueues::HasPendingTasksUnlocked(
    TaskQueueId queue_id) const {
  const auto& entry = queue_entries_.at(queue_id);
  bool is_subsumed = entry->subsumed_by != _kUnmerged;
  if (is_subsumed) {
    return false;
  }

  if (!entry->task_source->IsEmpty()) {
    return true;
  }

  auto &subsumed_set = entry->owner_of;
  for (TaskQueueId subsumed: subsumed_set) {
    if (!queue_entries_.at(subsumed)->task_source->IsEmpty()) {
      return true;
    }
  }
  return false;
}

fml::TimePoint MessageLoopTaskQueues::GetNextWakeTimeUnlocked(
    TaskQueueId queue_id) const {
  return PeekNextTaskUnlocked(queue_id).task.GetTargetTime();
}

TaskSource::TopTask MessageLoopTaskQueues::PeekNextTaskUnlocked(
    TaskQueueId owner) const {
  FML_DCHECK(HasPendingTasksUnlocked(owner));
  const auto& entry = queue_entries_.at(owner);
  if (entry->owner_of.empty()) {
    FML_LOG(ERROR) << "---- eggfly ---- PeekNextTaskUnlocked() entry->owner_of.empty()";
    if (fml::MessageLoop::IsInitializedForCurrentThread()) {
      FML_LOG(ERROR) << "---- eggfly ---- current=" << fml::MessageLoop::GetCurrentTaskQueueId();
    }
    return entry->task_source->Top();
  }

  TaskSource* owner_tasks = entry->task_source.get();
  std::vector<TaskSource::TopTask> candidate_top_tasks;

  if(!owner_tasks->IsEmpty()) {
    FML_LOG(ERROR)
    << "---- eggfly ---- PeekNextTaskUnlocked() owner_tasks.size=" << owner_tasks->GetNumPendingTasks();
    if (fml::MessageLoop::IsInitializedForCurrentThread()) {
      FML_LOG(ERROR) << "---- eggfly ---- current=" << fml::MessageLoop::GetCurrentTaskQueueId();
    }
    candidate_top_tasks.push_back(owner_tasks->Top());
  }
  for (TaskQueueId subsumed: entry->owner_of) {
    TaskSource* subsumed_tasks = queue_entries_.at(subsumed)->task_source.get();
    if (!subsumed_tasks->IsEmpty()) {
      FML_LOG(ERROR)
      << "---- eggfly ---- PeekNextTaskUnlocked() subsumed_tasks.size=" << subsumed_tasks->GetNumPendingTasks();
      if (fml::MessageLoop::IsInitializedForCurrentThread()) {
        FML_LOG(ERROR) << "---- eggfly ---- current=" << fml::MessageLoop::GetCurrentTaskQueueId();
      }
      candidate_top_tasks.push_back(subsumed_tasks->Top());
    }
  }
  // At least one task at the top because PeekNextTaskUnlocked() is called after HasPendingTasksUnlocked()
  FML_DCHECK(!candidate_top_tasks.empty());
  FML_LOG(ERROR)
  << "---- eggfly ---- PeekNextTaskUnlocked() candidate_top_tasks.size=" << candidate_top_tasks.size();
  if (fml::MessageLoop::IsInitializedForCurrentThread()) {
    FML_LOG(ERROR) << "---- eggfly ---- current=" << fml::MessageLoop::GetCurrentTaskQueueId();
  }
  TaskSource::TopTask &top = *std::max_element(candidate_top_tasks.begin(), candidate_top_tasks.end());
  return top;
}

}  // namespace fml
