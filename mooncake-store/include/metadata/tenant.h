#pragma once

// Tenant: one tenant's object route, the list of its objects with work in
// flight, and the namespace policy over it, if any. Replica-action leases and
// promotion candidates belong to their own subsystems, which hold their own
// state and validate it against the object the route holds before acting.
//
// The route (route::ObjectRoute) owns the keys, their locks, the objects and
// the group index; the tenant keeps the in-flight list in step with every
// write guard the route releases, and reports each release to the policy.

#include <array>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ylt/util/tl/expected.hpp>

#include "common/intrusive_list.h"
#include "common/transparent_string_hash.h"
#include "metadata/namespace_policy.h"
#include "route/object_route.h"
#include "tenant_id.h"
#include "types.h"

namespace mooncake {
namespace metadata {

class Tenant final : private route::RouteObserver {
   public:
    // `policy`, when set, outlives the tenant; `attachment` is what it hung on
    // this tenant when it was created.
    explicit Tenant(TenantId id = {}, NamespacePolicy* policy = nullptr,
                    std::unique_ptr<PolicyAttachment> attachment = nullptr)
        : objects(this),
          id_(std::move(id)),
          policy_(policy),
          attachment_(std::move(attachment)) {}
    Tenant(const Tenant&) = delete;
    Tenant& operator=(const Tenant&) = delete;

    route::ObjectRoute objects;

    const TenantId& id() const { return id_; }

    // The policy's say on the object `guard` holds taking `bytes` more memory,
    // asked before the memory is allocated; zero bytes asks only whether the
    // tenant admits writes. Always granted without a policy.
    [[nodiscard]] tl::expected<void, ErrorCode> Grow(
        const route::WriteGuard& guard, uint64_t bytes) const {
        if (policy_ == nullptr) {
            return {};
        }
        return policy_->OnGrow(PolicyContext{id_, attachment_.get(), guard},
                               bytes);
    }

    // Tell the policy the object `guard` holds is about to be replaced under
    // this same lock, or torn down for good; see NamespacePolicy::OnReplace
    // and OnTearDown.
    void BeforeReplace(const route::WriteGuard& guard) const {
        if (policy_ != nullptr) {
            policy_->OnReplace(PolicyContext{id_, attachment_.get(), guard});
        }
    }
    void BeforeTearDown(const route::WriteGuard& guard) const {
        if (policy_ != nullptr) {
            policy_->OnTearDown(PolicyContext{id_, attachment_.get(), guard});
        }
    }

    PolicyAttachment* policy_attachment() const { return attachment_.get(); }

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
    void OnWriteRelease(const route::WriteGuard& guard) override {
        SyncInFlight(guard);
        if (policy_ != nullptr) {
            policy_->OnWriteRelease(
                PolicyContext{id_, attachment_.get(), guard});
        }
    }

    // Puts the held key on the in-flight list or takes it off, to match
    // whether it holds an object that carries work. The stripe lock is taken
    // only when that changed.
    void SyncInFlight(const route::WriteGuard& guard) {
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

    const TenantId id_;
    NamespacePolicy* const policy_;
    const std::unique_ptr<PolicyAttachment> attachment_;

    // The keys with work in flight. Declared after the route so it is
    // destroyed first: the route's slots must outlive the hooks the list
    // holds.
    std::array<InFlightStripe, kInFlightStripeCount> in_flight_;
};

}  // namespace metadata
}  // namespace mooncake
