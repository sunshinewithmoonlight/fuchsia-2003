// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/ledger/bin/storage/impl/commit_pruner.h"

#include "src/ledger/lib/coroutine/coroutine_waiter.h"
#include "src/lib/callback/waiter.h"

namespace storage {
namespace {
// Extracts the parents of commits with the highest generation in |frontier|, removing those commits
// from |frontier| and returning their parents in |parents|.
Status ExploreGeneration(
    coroutine::CoroutineHandler* handler, CommitPruner::CommitPrunerDelegate* delegate,
    std::set<std::unique_ptr<const Commit>, storage::GenerationComparator>* frontier,
    std::vector<std::unique_ptr<const Commit>>* parents) {
  uint64_t expected_generation = (*frontier->begin())->GetGeneration();
  auto waiter =
      fxl::MakeRefCounted<callback::Waiter<Status, std::unique_ptr<const Commit>>>(Status::OK);
  while (!frontier->empty() && expected_generation == (*frontier->begin())->GetGeneration()) {
    // Pop the newest commit.
    const std::unique_ptr<const Commit>& commit = *frontier->begin();
    // Request its parents.
    for (const auto& parent_id : commit->GetParentIds()) {
      delegate->GetCommit(parent_id, waiter->NewCallback());
    }
    frontier->erase(frontier->begin());
  }
  Status status;
  if (coroutine::Wait(handler, std::move(waiter), &status, parents) ==
      coroutine::ContinuationStatus::INTERRUPTED) {
    return Status::INTERRUPTED;
  }
  return status;
}
}  // namespace

CommitPruner::CommitPruner(CommitPrunerDelegate* delegate, LiveCommitTracker* commit_tracker,
                           CommitPruningPolicy policy)
    : delegate_(delegate), commit_tracker_(commit_tracker), policy_(policy) {}

CommitPruner::~CommitPruner() = default;

Status CommitPruner::Prune(coroutine::CoroutineHandler* handler) {
  if (policy_ == CommitPruningPolicy::NEVER) {
    return Status::OK;
  }

  FXL_DCHECK(policy_ == CommitPruningPolicy::LOCAL_IMMEDIATE);

  std::unique_ptr<const storage::Commit> luca;
  RETURN_ON_ERROR(FindLatestUniqueCommonAncestorSync(handler, &luca));
  ClockEntry clock_entry{luca->GetId(), luca->GetGeneration()};

  RETURN_ON_ERROR(delegate_->UpdateSelfClockEntry(handler, clock_entry));

  std::vector<std::unique_ptr<const Commit>> commits;
  RETURN_ON_ERROR(GetAllAncestors(handler, std::move(luca), &commits));
  if (commits.empty()) {
    return Status::OK;
  }
  return delegate_->DeleteCommits(handler, std::move(commits));
}

// The algorithm goes as follows: we keep a set of "active" commits, ordered
// by generation order. Until this set has only one element, we take the
// commit with the greater generation (the one deepest in the commit graph)
// and replace it by its parent. If we seed the initial set with two commits,
// we get their unique lowest common ancestor.
// At each step of the iteration we request the parent commits of all commits
// with the same generation.
Status CommitPruner::FindLatestUniqueCommonAncestorSync(
    coroutine::CoroutineHandler* handler, std::unique_ptr<const storage::Commit>* result) {
  auto live_commits = commit_tracker_->GetLiveCommits();
  std::set<std::unique_ptr<const storage::Commit>, storage::GenerationComparator> commits(
      std::move_iterator(live_commits.begin()), std::move_iterator(live_commits.end()));

  while (commits.size() > 1) {
    // Pop the newest commits and retrieve their parents.
    std::vector<std::unique_ptr<const Commit>> parents;
    RETURN_ON_ERROR(ExploreGeneration(handler, delegate_, &commits, &parents));
    // Once the parents have been retrieved, add these in the set.
    for (auto& parent : parents) {
      commits.insert(std::move(parent));
    }
  }
  FXL_DCHECK(commits.size() == 1);
  *result = std::move(commits.extract(commits.begin()).value());
  return Status::OK;
}

Status CommitPruner::GetAllAncestors(coroutine::CoroutineHandler* handler,
                                     std::unique_ptr<const Commit> base,
                                     std::vector<std::unique_ptr<const Commit>>* result) {
  std::set<std::unique_ptr<const Commit>, storage::GenerationComparator> frontier;
  frontier.insert(std::move(base));
  std::set<std::unique_ptr<const Commit>, storage::GenerationComparator> ancestor_set;

  while (!frontier.empty()) {
    std::vector<std::unique_ptr<const Commit>> parents;
    Status status = ExploreGeneration(handler, delegate_, &frontier, &parents);
    if (status == Status::INTERNAL_NOT_FOUND) {
      // We are at the end of the commits to be pruned (the other commits are already pruned), exit
      // the loop.
      break;
    }
    RETURN_ON_ERROR(status);

    for (auto& parent : parents) {
      ancestor_set.insert(parent->Clone());
      frontier.insert(std::move(parent));
    }
  }

  result->clear();
  result->reserve(ancestor_set.size());
  for (auto it = ancestor_set.begin(); it != ancestor_set.end();) {
    result->push_back(std::move(ancestor_set.extract(it++).value()));
  }
  return Status::OK;
}

}  // namespace storage
