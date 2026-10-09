// object_test_helpers.h
//
// Builders shared by the per-object metadata suites.

#pragma once

#include <chrono>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "object_metadata.h"
#include "route/object_route.h"

namespace mooncake {
namespace test {

// A minimal 128 B, replica-less envelope. These suites exercise the object
// model, not replica validity, so nothing here sets a real replica. The write
// time is fixed rather than read from the clock so a suite that ages a lease
// cannot pick up a moving value.
inline std::unique_ptr<ObjectMetadata> MakeObjectMetadata(
    const std::string& user_key, const std::string& group_id = {}) {
    constexpr auto kWriteTime =
        std::chrono::system_clock::time_point(std::chrono::seconds(1));
    return std::make_unique<ObjectMetadata>(
        UUID{1, 2}, kWriteTime, 128, std::vector<Replica>{}, std::nullopt,
        false, ObjectDataType::UNKNOWN, group_id, TenantId(), user_key);
}

// Publishes that envelope under `key` and returns its generation, or 0 when
// the key already holds an object (the route never hands out generation 0).
inline route::Generation PublishObject(route::ObjectRoute& route,
                                       const std::string& key,
                                       const std::string& group_id = {}) {
    auto guard = route.WriteOrCreate(key);
    if (guard.has_object()) {
        return 0;
    }
    return guard.Publish(MakeObjectMetadata(key, group_id));
}

// Reaches beneath the route's public surface: for the route's own suite,
// which counts slots and keeps one referenced, and for fixtures that need a
// restored route's state, such as a group table that starts empty.
struct ObjectRouteTestPeer {
    // Every slot in the stripes, whether or not it holds an object.
    static size_t SlotCount(const route::ObjectRoute& route) {
        size_t count = 0;
        for (const auto& stripe : route.stripes_) {
            std::shared_lock<std::shared_mutex> lock(stripe.lock);
            count += stripe.slots.size();
        }
        return count;
    }

    // A strong reference to `key`'s slot, or null when the route has none.
    // Holding it keeps an empty slot from being collected.
    static std::shared_ptr<route::KeySlot> SlotRef(
        const route::ObjectRoute& route, std::string_view key) {
        return route.FindSlot(key);
    }

    // Drops every grouped object's membership from the group index, leaving
    // the objects (and the leases their metadata holds) as they are: the
    // state a restore starts from before RebuildGroupState(). The slots are
    // gathered under the stripe locks and locked only after those are let go,
    // since nothing waits for a slot lock while holding a stripe.
    static void DropGroupMemberships(route::ObjectRoute& route) {
        std::vector<std::shared_ptr<route::KeySlot>> slots;
        for (const auto& stripe : route.stripes_) {
            std::shared_lock<std::shared_mutex> lock(stripe.lock);
            for (const auto& entry : stripe.slots) {
                slots.push_back(entry.second);
            }
        }
        for (const auto& slot : slots) {
            std::unique_lock<std::shared_mutex> lock(slot->mutex_);
            if (!slot->record_.has_value()) {
                continue;
            }
            const std::string& group_id = slot->record_->metadata->group_id;
            if (!group_id.empty()) {
                (void)route.groups_.RemoveMember(group_id, slot->key());
            }
        }
    }
};

}  // namespace test
}  // namespace mooncake
