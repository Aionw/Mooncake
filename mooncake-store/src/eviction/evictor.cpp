#include "eviction/evictor.h"

#include <glog/logging.h>

#include <algorithm>
#include <cmath>
#include <optional>
#include <utility>

namespace mooncake {
namespace eviction {

Goal Goal::ShareOfFiled(double target_ratio, double floor_ratio) {
    if (target_ratio < floor_ratio) {
        LOG(ERROR) << "evict_ratio_target=" << target_ratio
                   << ", evict_ratio_lowerbound=" << floor_ratio
                   << ", error=invalid_params";
        floor_ratio = target_ratio;
    }
    Goal goal;
    goal.unit_ = Unit::kObjects;
    goal.target_ratio_ = target_ratio;
    goal.floor_ratio_ = floor_ratio;
    return goal;
}

Goal Goal::Bytes(uint64_t bytes) {
    Goal goal;
    goal.unit_ = Unit::kBytes;
    goal.bytes_ = bytes;
    return goal;
}

bool Goal::MetAt(const Report& report, int level) const {
    if (unit_ == Unit::kBytes) {
        return report.released_bytes >= bytes_;
    }
    // A group goes all or none, so a run may overshoot by one group.
    const double ratio = level == 0 ? target_ratio_ : floor_ratio_;
    const auto wanted = static_cast<uint64_t>(std::ceil(report.filed * ratio));
    const uint64_t counted =
        report.evicted_objects + (level == 0 ? 0 : credit_);
    return counted >= wanted;
}

// One run's work: the clock leases are judged against, the report it adds
// to, and the replicas taken off objects, which are freed once each
// candidate's locks are let go.
class Evictor::Pass {
   public:
    Pass(const Evictor& evictor, Report& report, Clock::time_point now)
        : evictor_(evictor), report_(report), now_(now) {}

    // Visits the keys the namespaces' indexes file, lowest rank first across
    // all of them, and reclaims each that qualifies at `level`, until `goal`
    // is met for it, an OpLog write fails, or no key is left. The first level
    // also counts what the indexes filed.
    void RunLevel(const std::vector<metadata::Namespace*>& namespaces,
                  int level, const Goal& goal) {
        struct Source {
            metadata::Namespace* ns;
            std::unique_ptr<metadata::EvictionIndex::Cursor> cursor;
        };
        // The next key of each source, kept as a min-heap by rank; a tie
        // goes to the earlier source.
        struct Head {
            metadata::EvictionIndex::Entry entry;
            size_t source;
        };
        const auto later = [](const Head& a, const Head& b) {
            return a.entry.rank != b.entry.rank ? a.entry.rank > b.entry.rank
                                                : a.source > b.source;
        };

        std::vector<Source> sources;
        sources.reserve(namespaces.size());
        std::vector<Head> heads;
        for (metadata::Namespace* ns : namespaces) {
            auto cursor = ns->eviction_index().Scan(now_);
            if (level == 0) {
                report_.filed += cursor->Total();
            }
            if (auto entry = cursor->Next()) {
                heads.push_back({std::move(*entry), sources.size()});
            }
            sources.push_back({ns, std::move(cursor)});
        }
        std::make_heap(heads.begin(), heads.end(), later);

        while (!heads.empty() && !report_.stopped &&
               !goal.MetAt(report_, level)) {
            std::pop_heap(heads.begin(), heads.end(), later);
            Head head = std::move(heads.back());
            heads.pop_back();
            Source& source = sources[head.source];
            Visit(*source.ns, *source.cursor, head.entry, level);
            if (auto entry = source.cursor->Next()) {
                heads.push_back({std::move(*entry), head.source});
                std::push_heap(heads.begin(), heads.end(), later);
            }
        }
    }

   private:
    // Reclaims the object `entry` names, or the whole group it belongs to,
    // and frees what that took once every lock is let go.
    void Visit(metadata::Namespace& ns,
               const metadata::EvictionIndex::Cursor& cursor,
               const metadata::EvictionIndex::Entry& entry, int level) {
        VisitLocked(ns, cursor, entry, level);
        detached_.clear();
    }

    void VisitLocked(metadata::Namespace& ns,
                     const metadata::EvictionIndex::Cursor& cursor,
                     const metadata::EvictionIndex::Entry& entry, int level) {
        // Grouped-ness is read under the object's own lock, which is let go
        // before the group's members are visited one at a time.
        std::string group_id;
        {
            auto hold = ns.objects.Write(entry.key);
            if (!hold) {
                return;
            }
            // Filed ahead of its real rank: letting the lock go files it
            // there, where the walk meets it in its turn.
            if (!cursor.StillAt(*hold, entry)) {
                Skip(ns, *hold, SkipReason::kMoved);
                return;
            }
            auto plan = Judge(ns, *hold, level);
            if (!plan) {
                return;
            }
            if (!hold->metadata().IsGrouped()) {
                if (Reclaim(ns, *hold, *plan)) {
                    evictor_.reclaimer_.TearDown(ns, *hold);
                }
                return;
            }
            group_id = hold->metadata().group_id;
        }
        VisitGroup(ns, entry.key, group_id, level);
    }

    // Reclaims every member of `group_id` that qualifies, or `trigger_key`
    // alone when the group has no members. The member list is a snapshot, so
    // each member is found and judged again under its own lock; those left
    // with nothing valid are torn down once that lock is let go, under one
    // taken again, and only while they still hold the same publication.
    void VisitGroup(metadata::Namespace& ns, const std::string& trigger_key,
                    const std::string& group_id, int level) {
        std::vector<std::string> member_keys =
            ns.objects.GroupMembers(group_id);
        if (member_keys.empty()) {
            member_keys.push_back(trigger_key);
        }
        std::vector<route::ObjectRef> to_tear_down;
        for (const auto& member_key : member_keys) {
            if (report_.stopped) {
                break;
            }
            auto hold = ns.objects.Write(member_key);
            if (!hold) {
                continue;
            }
            auto plan = Judge(ns, *hold, level);
            if (plan && Reclaim(ns, *hold, *plan)) {
                to_tear_down.push_back(hold->ref());
            }
        }
        for (const auto& member : to_tear_down) {
            auto hold = ns.objects.Write(member);
            if (hold && !hold->metadata().IsValid()) {
                evictor_.reclaimer_.TearDown(ns, *hold);
            }
        }
    }

    // The plan for the object `hold` holds at `level`, or why it is skipped.
    tl::expected<Plan, SkipReason> Judge(metadata::Namespace& ns,
                                         const route::WriteGuard& hold,
                                         int level) {
        const ObjectMetadata& metadata = hold.metadata();
        // Not const: ObjectMetadata::IsLeaseExpired takes it by reference.
        Clock::time_point now = now_;
        SkipReason reason;
        if (metadata.IsHardPinned()) {
            reason = SkipReason::kHardPinned;
        } else if (!metadata.IsLeaseExpired(now)) {
            // A surviving shared group lease keeps every member alive.
            reason = SkipReason::kLeaseActive;
        } else {
            auto plan = evictor_.policy_.Decide(
                Candidate{ns.id(), hold.key(), metadata, now_, level});
            if (plan) {
                return plan;
            }
            reason = plan.error();
        }
        Skip(ns, hold, reason);
        return tl::make_unexpected(reason);
    }

    // Carries out `plan` on the object `hold` holds and adds what it did to
    // the report. Returns whether the object is left to tear down.
    bool Reclaim(metadata::Namespace& ns, const route::WriteGuard& hold,
                 const Plan& plan) {
        auto effect = evictor_.reclaimer_.Apply(ns, hold, plan, report_);
        if (!effect) {
            report_.stopped = true;
            return false;
        }
        report_.Add(*effect);
        const ObjectView view{ns.id(), hold.key(), hold.metadata()};
        for (EvictionObserver* observer : evictor_.observers_) {
            observer->OnApplied(view, *effect);
        }
        if (!effect->detached.empty()) {
            detached_.push_back(std::move(effect->detached));
        }
        return effect->needs_teardown;
    }

    void Skip(metadata::Namespace& ns, const route::WriteGuard& hold,
              SkipReason reason) {
        const ObjectView view{ns.id(), hold.key(), hold.metadata()};
        for (EvictionObserver* observer : evictor_.observers_) {
            observer->OnSkipped(view, reason);
        }
    }

    const Evictor& evictor_;
    Report& report_;
    const Clock::time_point now_;
    std::vector<std::vector<Replica>> detached_;
};

Evictor::Evictor(metadata::NamespaceTable& namespaces,
                 const EvictionPolicy& policy, Reclaimer& reclaimer,
                 std::vector<EvictionObserver*> observers)
    : namespaces_(namespaces),
      policy_(policy),
      reclaimer_(reclaimer),
      observers_(std::move(observers)) {}

Report Evictor::Run(const Scope& scope, const Goal& goal) {
    Report report;
    report.credit = goal.credit();

    std::vector<metadata::Namespace*> namespaces;
    if (const TenantId* tenant_id = scope.tenant_id()) {
        // A tenant with no namespace owns no object.
        if (metadata::Namespace* ns = namespaces_.Lookup(*tenant_id)) {
            namespaces.push_back(ns);
        }
    } else {
        namespaces_.Visit([&](const TenantId&, metadata::Namespace& ns) {
            namespaces.push_back(&ns);
        });
    }

    Pass pass(*this, report, Clock::now());
    const int levels = policy_.Levels();
    for (int level = 0; level < levels; ++level) {
        if (level > 0 && (report.filed == 0 || report.stopped ||
                          goal.MetAt(report, level))) {
            break;
        }
        pass.RunLevel(namespaces, level, goal);
    }

    for (EvictionObserver* observer : observers_) {
        observer->OnRunEnd(scope, report);
    }
    return report;
}

}  // namespace eviction
}  // namespace mooncake
