#pragma once

// EvictionPolicy: which objects eviction may take memory from, and which of
// their replicas it takes. The evictor enforces what no policy may override,
// that a hard-pinned object or one whose lease has not run out is left alone,
// and asks the policy about the rest.
//
// A policy may go about it in levels, each more aggressive than the last (a
// soft pin binds at level 0 and gives at level 1, say): a run moves to the
// next level only while the goal is not met.

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include <ylt/util/tl/expected.hpp>

#include "object_metadata.h"
#include "replica.h"
#include "tenant_id.h"

namespace mooncake {
namespace eviction {

using Clock = std::chrono::system_clock;

// An object the evictor may take memory from, as the policy sees it under the
// object's write lock.
struct Candidate {
    const TenantId& tenant_id;
    const std::string& key;
    const ObjectMetadata& metadata;
    Clock::time_point now;
    int level;
};

// The replicas to take off the candidate.
struct Plan {
    std::vector<ReplicaID> release;
};

enum class SkipReason : uint8_t {
    // The index filed the key ahead of its real rank.
    kMoved,
    kHardPinned,
    kLeaseActive,
    kSoftPinned,
    // No replica the policy would release.
    kNothingToRelease,
};

class EvictionPolicy {
   public:
    // How many levels a run may go through; at least one.
    [[nodiscard]] virtual int Levels() const = 0;
    // The replicas to release from `candidate`, or why it is skipped. It may
    // not change the object.
    [[nodiscard]] virtual tl::expected<Plan, SkipReason> Decide(
        const Candidate& candidate) const = 0;

   protected:
    ~EvictionPolicy() = default;
};

}  // namespace eviction
}  // namespace mooncake
