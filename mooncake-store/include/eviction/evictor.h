#pragma once

// Evictor: frees memory by taking replicas off objects whose lease has run
// out, for the whole pool or for one namespace.
//
// The evictor runs the loop and keeps its invariants; what varies is
// injected:
//
// - Each namespace's EvictionIndex decides the order objects are visited in.
//   The evictor merges their cursors by rank.
// - The EvictionPolicy decides which objects qualify and which of their
//   replicas go, level by level.
// - The Reclaimer takes those replicas off and says what came of it.
// - EvictionObservers are told what happened and change nothing.
//
// What no policy overrides: a hard-pinned object, or one whose lease has not
// run out, is left alone; a group goes by its shared lease, its members judged
// one at a time; an object reclaiming left with nothing valid is torn down, a
// group member only once the group walk is over and while its key still holds
// the publication that was judged.
//
// Lock order: the caller holds snapshot_mutex_ shared for the whole run. The
// evictor holds at most one key lock at a time, and frees the replicas it took
// only once that lock is let go.

#include <cstdint>
#include <vector>

#include "eviction/observer.h"
#include "eviction/policy.h"
#include "eviction/reclaimer.h"
#include "eviction/report.h"
#include "metadata/namespace_table.h"

namespace mooncake {
namespace eviction {

// When a run stops. Level 0 stops once its target is met; the levels after it
// run only while the floor is not, and the credit counts toward the floor.
class Goal {
   public:
    // A share of the objects the indexes filed when the run began, counted
    // in objects that released memory. A floor above the target is lowered
    // to it.
    static Goal ShareOfFiled(double target_ratio, double floor_ratio);
    // Released bytes.
    static Goal Bytes(uint64_t bytes);

    // `credit` more counted toward the floor: what the caller freed on its
    // own just before the run.
    [[nodiscard]] Goal WithCredit(uint64_t credit) const {
        Goal goal = *this;
        goal.credit_ = credit;
        return goal;
    }

    [[nodiscard]] uint64_t credit() const { return credit_; }
    // Whether `report` meets what `level` aims for.
    [[nodiscard]] bool MetAt(const Report& report, int level) const;

   private:
    enum class Unit : uint8_t { kObjects, kBytes };

    Goal() = default;

    Unit unit_ = Unit::kObjects;
    double target_ratio_ = 0;
    double floor_ratio_ = 0;
    uint64_t bytes_ = 0;
    uint64_t credit_ = 0;
};

class Evictor {
   public:
    // Everything given outlives the evictor.
    Evictor(metadata::NamespaceTable& namespaces, const EvictionPolicy& policy,
            Reclaimer& reclaimer, std::vector<EvictionObserver*> observers);
    Evictor(const Evictor&) = delete;
    Evictor& operator=(const Evictor&) = delete;

    // Evicts within `scope` until `goal` is met, nothing more qualifies, or an
    // OpLog write fails. Namespaces created during the run join the next one.
    Report Run(const Scope& scope, const Goal& goal);

   private:
    class Pass;

    metadata::NamespaceTable& namespaces_;
    const EvictionPolicy& policy_;
    Reclaimer& reclaimer_;
    const std::vector<EvictionObserver*> observers_;
};

}  // namespace eviction
}  // namespace mooncake
