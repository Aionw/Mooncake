#pragma once

// Tenant: one tenant's object route and the list of its objects with work in
// flight. Quota, replica-action leases and promotion candidates belong to
// their own subsystems, which hold their own state and validate it against the
// object the route holds before acting.
//
// The route (route::ObjectRoute) owns the keys, their locks, the objects and
// the group index; the tenant only adds the in-flight list, which it keeps in
// step with every write guard the route releases.

#include <array>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "common/intrusive_list.h"
#include "common/transparent_string_hash.h"
#include "route/object_route.h"

namespace mooncake {
namespace metadata {

class Tenant final : private route::RouteObserver {
   public:
    Tenant() : objects(this) {}
    Tenant(const Tenant&) = delete;
    Tenant& operator=(const Tenant&) = delete;

    route::ObjectRoute objects;

    // True when the tenant holds no object and no group membership.
    [[nodiscard]] bool Empty() const { return objects.Empty(); }

    // --- Work in flight ------------------------------------------------------
    //
    // The tenant lists the keys whose object carries work in flight (see
    // route::ObjectState::HasInFlightWork), so a sweep for expired work walks
    // those instead of every object. Work only starts or finishes under a write
    // guard, and the route reports every write guard to the tenant as it is
    // released: a key is listed exactly while it holds an object that carries
    // work as of the last write guard on it. A write cursor neither starts nor
    // finishes such work.

    // The listed keys, as a snapshot to resolve again: an object can finish,
    // be torn down or be replaced once the stripe lock is released.
    [[nodiscard]] std::vector<std::string> InFlightKeys() const {
        std::vector<std::string> keys;
        for (const InFlightStripe& stripe : in_flight_) {
            std::lock_guard<std::mutex> lock(stripe.lock);
            for (const route::KeySlot& slot : stripe.slots) {
                keys.push_back(slot.key());
            }
        }
        return keys;
    }

   private:
    // Puts the held key on the in-flight list or takes it off, to match
    // whether it holds an object that carries work. The stripe lock is taken
    // only when that changed.
    void OnWriteRelease(const route::WriteGuard& guard) override {
        route::SlotOwnerState& owner = guard.owner_state();
        const bool listed =
            guard.has_object() && guard.state().HasInFlightWork();
        if (listed == owner.in_flight_listed) {
            return;
        }
        InFlightStripe& stripe = InFlightStripeOf(guard.key());
        std::lock_guard<std::mutex> lock(stripe.lock);
        if (listed) {
            stripe.slots.PushBack(guard.slot());
        } else {
            stripe.slots.Erase(guard.slot());
        }
        owner.in_flight_listed = listed;
    }

    using InFlightList = IntrusiveList<route::KeySlot, InFlightListTag>;

    // One stripe of the in-flight list. Starting and finishing a write each
    // touch the list, so it is striped by key, as the route is, rather than
    // put under one lock for the whole tenant.
    struct InFlightStripe {
        mutable std::mutex lock;
        InFlightList slots;
    };

    static constexpr size_t kInFlightStripeCount =
        route::ObjectRoute::kStripeCount;

    InFlightStripe& InFlightStripeOf(std::string_view key) {
        return in_flight_[TransparentStringHash{}(key) % kInFlightStripeCount];
    }

    // The keys with work in flight. Declared after the route so it is
    // destroyed first: the route's slots must outlive the hooks the list
    // holds.
    std::array<InFlightStripe, kInFlightStripeCount> in_flight_;
};

}  // namespace metadata
}  // namespace mooncake
