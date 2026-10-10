#include "eviction/evictor.h"

#include <algorithm>
#include <chrono>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "allocator.h"
#include "eviction/offloading_reclaimer.h"
#include "metadata/namespace_table.h"

namespace mooncake {
namespace {

using eviction::Clock;
using eviction::Goal;
using eviction::Outcome;
using eviction::Scope;
using eviction::SkipReason;
using std::chrono::seconds;

bool InPlan(const eviction::Plan& plan, const Replica& replica) {
    return std::find(plan.release.begin(), plan.release.end(), replica.id()) !=
           plan.release.end();
}

// Every completed memory replica goes; a soft-pinned key gives only past
// level 0.
class FakePolicy final : public eviction::EvictionPolicy {
   public:
    int Levels() const override { return levels; }

    tl::expected<eviction::Plan, SkipReason> Decide(
        const eviction::Candidate& candidate) const override {
        if (candidate.level == 0 && soft_pinned.count(candidate.key) > 0) {
            return tl::make_unexpected(SkipReason::kSoftPinned);
        }
        eviction::Plan plan;
        for (const Replica& replica : candidate.metadata.GetAllReplicas()) {
            if (replica.is_memory_replica() && replica.is_completed()) {
                plan.release.push_back(replica.id());
            }
        }
        if (plan.release.empty()) {
            return tl::make_unexpected(SkipReason::kNothingToRelease);
        }
        return plan;
    }

    int levels = 1;
    std::set<std::string> soft_pinned;
};

// Takes the planned replicas off on the spot and records what it did; fails
// the `fail_at`-th Apply, counting from zero, as an OpLog write would.
class FakeReclaimer final : public eviction::Reclaimer {
   public:
    tl::expected<eviction::Effect, ErrorCode> Apply(
        metadata::Namespace&, const route::WriteGuard& hold,
        const eviction::Plan& plan, const eviction::Report&) override {
        if (static_cast<int>(applies++) == fail_at) {
            return tl::make_unexpected(ErrorCode::INTERNAL_ERROR);
        }
        applied.push_back(hold.key());
        ObjectMetadata& metadata = hold.metadata();
        eviction::Effect effect;
        effect.detached = metadata.PopReplicas(
            [&plan](const Replica& replica) { return InPlan(plan, replica); });
        effect.released_bytes = metadata.size * effect.detached.size();
        effect.needs_teardown = !metadata.IsValid();
        effect.outcome =
            effect.needs_teardown ? Outcome::kDropped : Outcome::kDemoted;
        return effect;
    }

    void TearDown(metadata::Namespace&,
                  const route::WriteGuard& hold) override {
        torn_down.push_back(hold.key());
        hold.TearDown();
    }

    int fail_at = -1;
    size_t applies = 0;
    std::vector<std::string> applied;
    std::vector<std::string> torn_down;
};

class RecordingObserver final : public eviction::EvictionObserver {
   public:
    void OnApplied(const eviction::ObjectView& view,
                   const eviction::Effect&) override {
        applied.push_back(view.key);
    }
    void OnSkipped(const eviction::ObjectView& view,
                   SkipReason reason) override {
        skipped.emplace_back(view.key, reason);
    }
    void OnRunEnd(const Scope&, const eviction::Report& report) override {
        ++runs;
        filed = report.filed;
    }

    std::vector<std::string> applied;
    std::vector<std::pair<std::string, SkipReason>> skipped;
    int runs = 0;
    uint64_t filed = 0;
};

class EvictorTest : public ::testing::Test {
   protected:
    // Publishes `key` in `tenant`'s namespace with `replicas` 128 B memory
    // replicas and its lease ending `age` ago.
    void Publish(const std::string& tenant, const std::string& key,
                 Clock::duration age, const std::string& group_id = {},
                 int replicas = 1) {
        metadata::Namespace& ns = namespaces_.GetOrCreate(TenantId(tenant));
        auto guard = ns.objects.WriteOrCreate(key);
        std::vector<Replica> list;
        for (int i = 0; i < replicas; ++i) {
            list.emplace_back(allocator_->allocate(128),
                              ReplicaStatus::COMPLETE);
        }
        (void)guard.Publish(
            UUID{1, 2}, Clock::time_point{}, 128, std::move(list), std::nullopt,
            false, ObjectDataType::UNKNOWN, group_id, TenantId(tenant), key);
        SpinLocker locker(&guard.metadata().lock);
        guard.metadata().lease_->SetDeadline(Clock::now() - age);
    }

    eviction::Report Run(const Goal& goal) {
        eviction::Evictor evictor(namespaces_, policy_, reclaimer_,
                                  {&observer_});
        return evictor.Run(Scope::All(), goal);
    }

    // Declared first, so it outlives the replicas it gave out.
    std::shared_ptr<OffsetBufferAllocator> allocator_ =
        std::make_shared<OffsetBufferAllocator>("segment", 0x100000000ULL,
                                                64 * 1024 * 1024, "segment");
    metadata::NamespaceTable namespaces_{[](const TenantId& tenant_id) {
        return std::make_unique<metadata::Namespace>(tenant_id);
    }};
    FakePolicy policy_;
    FakeReclaimer reclaimer_;
    RecordingObserver observer_;
};

TEST_F(EvictorTest, MergesNamespacesOldestLeaseFirst) {
    Publish("a", "a1", seconds(10));
    Publish("a", "a2", seconds(6));
    Publish("b", "b1", seconds(8));
    Publish("b", "b2", seconds(4));

    const auto report = Run(Goal::ShareOfFiled(1.0, 1.0));

    EXPECT_EQ(reclaimer_.applied,
              (std::vector<std::string>{"a1", "b1", "a2", "b2"}));
    EXPECT_EQ(reclaimer_.torn_down, reclaimer_.applied);
    EXPECT_EQ(report.filed, 4u);
    EXPECT_EQ(report.evicted_objects, 4u);
    EXPECT_EQ(report.released_bytes, 4u * 128);
    EXPECT_EQ(report.Count(Outcome::kDropped), 4u);
}

TEST_F(EvictorTest, StopsOnceTheTargetShareIsMet) {
    Publish("a", "k1", seconds(10));
    Publish("a", "k2", seconds(8));
    Publish("a", "k3", seconds(6));
    Publish("a", "k4", seconds(4));

    const auto report = Run(Goal::ShareOfFiled(0.5, 0.5));

    EXPECT_EQ(reclaimer_.applied, (std::vector<std::string>{"k1", "k2"}));
    EXPECT_EQ(report.filed, 4u);
    EXPECT_EQ(report.evicted_objects, 2u);
}

TEST_F(EvictorTest, BytesGoalCountsReleasedMemory) {
    Publish("a", "k1", seconds(10), {}, /*replicas=*/2);
    Publish("a", "k2", seconds(8));

    const auto report = Run(Goal::Bytes(200));

    EXPECT_EQ(reclaimer_.applied, std::vector<std::string>{"k1"});
    EXPECT_EQ(report.released_bytes, 256u);
}

TEST_F(EvictorTest, SoftPinnedGoOnlyAtTheNextLevelWhileShort) {
    Publish("a", "pinned", seconds(10));
    Publish("a", "plain", seconds(8));
    policy_.soft_pinned = {"pinned"};

    // One level: the soft pin holds.
    Run(Goal::ShareOfFiled(1.0, 1.0));
    EXPECT_EQ(reclaimer_.applied, std::vector<std::string>{"plain"});
    ASSERT_FALSE(observer_.skipped.empty());
    EXPECT_EQ(observer_.skipped.front(),
              std::make_pair(std::string("pinned"), SkipReason::kSoftPinned));

    // Two levels: still short of the floor, so the pinned one goes too.
    Publish("a", "plain", seconds(8));
    reclaimer_.applied.clear();
    policy_.levels = 2;
    Run(Goal::ShareOfFiled(1.0, 1.0));
    EXPECT_EQ(reclaimer_.applied,
              (std::vector<std::string>{"plain", "pinned"}));
}

TEST_F(EvictorTest, NextLevelSkippedOnceTheFloorIsMet) {
    Publish("a", "pinned", seconds(10));
    Publish("a", "plain", seconds(8));
    policy_.soft_pinned = {"pinned"};
    policy_.levels = 2;

    Run(Goal::ShareOfFiled(1.0, 0.5));

    EXPECT_EQ(reclaimer_.applied, std::vector<std::string>{"plain"});
}

TEST_F(EvictorTest, CreditCountsTowardTheFloorOnly) {
    Publish("a", "pinned", seconds(10));
    Publish("a", "plain", seconds(8));
    policy_.soft_pinned = {"pinned"};
    policy_.levels = 2;

    // Level 0 still evicts toward the target; the credit then makes up the
    // floor, so level 1 does not run.
    const auto report = Run(Goal::ShareOfFiled(1.0, 1.0).WithCredit(1));

    EXPECT_EQ(reclaimer_.applied, std::vector<std::string>{"plain"});
    EXPECT_TRUE(report.MadeProgress());
}

TEST_F(EvictorTest, AFailedReclaimStopsTheRun) {
    Publish("a", "k1", seconds(10));
    Publish("a", "k2", seconds(8));
    Publish("a", "k3", seconds(6));
    reclaimer_.fail_at = 1;

    const auto report = Run(Goal::ShareOfFiled(1.0, 1.0));

    EXPECT_TRUE(report.stopped);
    EXPECT_EQ(reclaimer_.applied, std::vector<std::string>{"k1"});
    EXPECT_EQ(reclaimer_.applies, 2u);
}

TEST_F(EvictorTest, AGroupGoesTogetherByItsSharedLease) {
    // Members share one lease, so the last one published sets it for both.
    Publish("a", "g1", seconds(10), "g");
    Publish("a", "g2", seconds(10), "g");
    Publish("a", "x", seconds(5));
    Publish("a", "y", seconds(3));

    // One object's worth of target, but the group goes whole.
    const auto report = Run(Goal::ShareOfFiled(0.25, 0.25));

    EXPECT_EQ(std::set<std::string>(reclaimer_.applied.begin(),
                                    reclaimer_.applied.end()),
              (std::set<std::string>{"g1", "g2"}));
    EXPECT_EQ(std::set<std::string>(reclaimer_.torn_down.begin(),
                                    reclaimer_.torn_down.end()),
              (std::set<std::string>{"g1", "g2"}));
    EXPECT_EQ(report.evicted_objects, 2u);
}

TEST_F(EvictorTest, ObserversHearOfEachObjectAndTheRun) {
    Publish("a", "k1", seconds(10));
    Publish("b", "k2", seconds(8));

    Run(Goal::ShareOfFiled(1.0, 1.0));

    EXPECT_EQ(observer_.applied, (std::vector<std::string>{"k1", "k2"}));
    EXPECT_EQ(observer_.runs, 1);
    EXPECT_EQ(observer_.filed, 2u);
}

TEST_F(EvictorTest, AScopedRunStaysInItsNamespace) {
    Publish("a", "a1", seconds(10));
    Publish("b", "b1", seconds(10));

    eviction::Evictor evictor(namespaces_, policy_, reclaimer_, {});
    const TenantId tenant("b");
    const auto report = evictor.Run(Scope::Of(tenant), Goal::Bytes(1 << 20));

    EXPECT_EQ(reclaimer_.applied, std::vector<std::string>{"b1"});
    EXPECT_EQ(report.filed, 1u);

    // A tenant with no namespace owns nothing to evict.
    const TenantId absent("absent");
    EXPECT_EQ(evictor.Run(Scope::Of(absent), Goal::Bytes(1)).filed, 0u);
}

// --- OffloadingReclaimer -----------------------------------------------------

// Accepts the first `capacity` offloads.
class FakeOffloadQueue final : public eviction::OffloadQueue {
   public:
    bool Push(const TenantId&, const std::string& key, Replica&,
              std::vector<UUID>* mirror_clients) override {
        if (pushed.size() >= capacity) {
            return false;
        }
        pushed.push_back(key);
        mirror_clients->push_back(UUID{7, 7});
        return true;
    }

    size_t capacity = SIZE_MAX;
    std::vector<std::string> pushed;
};

class OffloadingReclaimerTest : public EvictorTest {
   protected:
    tl::expected<eviction::Effect, ErrorCode> Apply(
        const std::string& key, eviction::OffloadingReclaimer::Options options,
        const eviction::Report& so_far = {}) {
        eviction::OffloadingReclaimer reclaimer(inner_, queue_, options);
        metadata::Namespace& ns = namespaces_.GetOrCreate(TenantId("a"));
        auto hold = ns.objects.Write(key);
        EXPECT_TRUE(hold.has_value());
        eviction::Candidate candidate{ns.id(), key, hold->metadata(),
                                      Clock::now(), 0};
        auto plan = policy_.Decide(candidate);
        EXPECT_TRUE(plan.has_value());
        auto effect = reclaimer.Apply(ns, *hold, *plan, so_far);
        if (effect && hold->state().offloading_task.has_value()) {
            offloading_source_ = hold->state().offloading_task->source_id;
        }
        return effect;
    }

    FakeReclaimer inner_;
    FakeOffloadQueue queue_;
    std::optional<ReplicaID> offloading_source_;
};

TEST_F(OffloadingReclaimerTest, PinsOneReplicaAndReleasesTheRest) {
    Publish("a", "k", seconds(10), {}, /*replicas=*/2);

    auto effect = Apply("k", {});

    ASSERT_TRUE(effect.has_value());
    EXPECT_EQ(effect->outcome, Outcome::kOffloadQueued);
    EXPECT_EQ(effect->offload_bytes, 128u);
    EXPECT_EQ(effect->released_bytes, 128u);
    EXPECT_FALSE(effect->needs_teardown);
    EXPECT_EQ(queue_.pushed, std::vector<std::string>{"k"});
    EXPECT_TRUE(offloading_source_.has_value());
}

TEST_F(OffloadingReclaimerTest, AnObjectOnLocalDiskIsDemotedWithoutOffload) {
    Publish("a", "k", seconds(10));
    {
        auto hold = namespaces_.GetOrCreate(TenantId("a")).objects.Write("k");
        std::vector<Replica> disk;
        disk.emplace_back(UUID{3, 4}, 128, "endpoint", ReplicaStatus::COMPLETE);
        hold->metadata().AddReplicas(std::move(disk));
    }

    auto effect = Apply("k", {});

    ASSERT_TRUE(effect.has_value());
    EXPECT_EQ(effect->outcome, Outcome::kDemoted);
    EXPECT_TRUE(queue_.pushed.empty());
}

// With the OpLog on, a local-disk replica being evicted stays attached,
// marked removed, until its removal is durable: it keeps nothing by then.
TEST_F(OffloadingReclaimerTest, ALocalDiskReplicaBeingRemovedStillNeedsOffload) {
    Publish("a", "k", seconds(10));
    {
        auto hold = namespaces_.GetOrCreate(TenantId("a")).objects.Write("k");
        std::vector<Replica> disk;
        disk.emplace_back(UUID{3, 4}, 128, "endpoint", ReplicaStatus::COMPLETE);
        disk.back().mark_removed();
        hold->metadata().AddReplicas(std::move(disk));
    }

    auto effect = Apply("k", {});

    ASSERT_TRUE(effect.has_value());
    EXPECT_EQ(effect->outcome, Outcome::kOffloadQueued);
    EXPECT_EQ(queue_.pushed, std::vector<std::string>{"k"});
}

TEST_F(OffloadingReclaimerTest, ARejectedOffloadKeepsTheObjectUnlessForced) {
    Publish("a", "kept", seconds(10));
    Publish("a", "forced", seconds(10));
    queue_.capacity = 0;

    auto kept = Apply("kept", {});
    ASSERT_TRUE(kept.has_value());
    EXPECT_EQ(kept->outcome, Outcome::kKept);
    EXPECT_TRUE(inner_.applied.empty());

    auto forced = Apply("forced", {.cap = 100, .force_evict = true});
    ASSERT_TRUE(forced.has_value());
    EXPECT_EQ(forced->outcome, Outcome::kDropped);
    EXPECT_EQ(forced->reason, eviction::DropReason::kOffloadRejected);
}

TEST_F(OffloadingReclaimerTest, PastTheCapAForcedRunDropsWithoutOffload) {
    Publish("a", "k", seconds(10));
    eviction::Report so_far;
    so_far.outcomes[static_cast<size_t>(Outcome::kOffloadQueued)] = 2;

    auto effect = Apply("k", {.cap = 2, .force_evict = true}, so_far);

    ASSERT_TRUE(effect.has_value());
    EXPECT_EQ(effect->outcome, Outcome::kDropped);
    EXPECT_EQ(effect->reason, eviction::DropReason::kOffloadCap);
    EXPECT_TRUE(queue_.pushed.empty());
}

}  // namespace
}  // namespace mooncake
