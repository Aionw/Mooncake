#pragma once

// TenantRegistry: tenant id -> that tenant's metadata, and the lifecycle
// rules MasterService resolves tenants through. A tenant is created
// atomically, published fully initialized, and from then on stays where it is
// for as long as the service serves: a reference a caller resolved never
// dangles, so it needs no ownership of its own.
//
// Every object operation resolves its tenant while tenants are created rarely,
// so the table is one map under a reader-writer lock: a lookup takes the lock
// shared and a create takes it exclusively. The default tenant, which every
// request resolves when multi-tenancy is off, is also kept in an atomic once
// created, so resolving it takes no lock and writes to no shared cache line.
// The registry locks only its own map: each tenant synchronizes its own
// containers.
//
// The factory is bound once at construction, so every tenant of one registry is
// built the same way and a caller only names the tenant it wants; it is where
// whatever a policy hangs on a tenant is resolved. It runs before the registry
// lock is taken, so it may take other locks without nesting them under this
// one, and a tenant is never reachable before it is fully built. Racing
// creators of one id may each run it, and only one result is published, so it
// must have no effect that cannot be repeated, and it must not re-enter this
// registry.

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <utility>
#include <vector>

#include "metadata/tenant.h"
#include "tenant_id.h"

namespace mooncake {
namespace metadata {

class TenantRegistry {
   public:
    // One tenant id in, one initialized tenant out.
    using TenantFactory =
        std::function<std::unique_ptr<Tenant>(const TenantId&)>;

    explicit TenantRegistry(TenantFactory factory)
        : factory_(std::move(factory)) {}

    // Null when the tenant is absent.
    [[nodiscard]] Tenant* Lookup(const TenantId& tenant_id) const {
        if (tenant_id.IsDefault()) {
            return default_.load(std::memory_order_acquire);
        }
        std::shared_lock<std::shared_mutex> lock(mutex_);
        const auto it = tenants_.find(tenant_id);
        return it == tenants_.end() ? nullptr : it->second.get();
    }

    // Atomic get-or-create: concurrent callers for the same tenant all observe
    // the one Tenant the winner published; a loser's build is dropped. The
    // lookup covers the common case, so the factory runs and the exclusive
    // lock is taken on a miss only.
    [[nodiscard]] Tenant& GetOrCreateTenant(const TenantId& tenant_id) {
        if (Tenant* tenant = Lookup(tenant_id)) {
            return *tenant;
        }
        auto candidate = factory_(tenant_id);
        std::unique_lock<std::shared_mutex> lock(mutex_);
        Tenant& tenant = *tenants_.try_emplace(tenant_id, std::move(candidate))
                              .first->second;
        if (tenant_id.IsDefault()) {
            default_.store(&tenant, std::memory_order_release);
        }
        return tenant;
    }

    // Drops every tenant. Only for a reset while no request runs (a restore
    // at startup): a tenant a caller still refers to would dangle.
    void Clear() {
        std::unordered_map<TenantId, std::unique_ptr<Tenant>, TenantIdHash>
            dropped;
        {
            std::unique_lock<std::shared_mutex> lock(mutex_);
            default_.store(nullptr, std::memory_order_release);
            dropped.swap(tenants_);
        }
    }

    // Walks the tenants present when the walk starts. They are collected
    // under the shared lock and `fn(tenant_id, tenant)` runs after it is
    // released, so `fn` may take the tenant's own locks and may create
    // tenants; a tenant created during the walk joins the next one.
    template <typename Fn>
    void Visit(Fn&& fn) const {
        std::vector<std::pair<const TenantId*, Tenant*>> tenants;
        {
            std::shared_lock<std::shared_mutex> lock(mutex_);
            tenants.reserve(tenants_.size());
            for (const auto& [tenant_id, tenant] : tenants_) {
                tenants.emplace_back(&tenant_id, tenant.get());
            }
        }
        for (const auto& [tenant_id, tenant] : tenants) {
            fn(*tenant_id, *tenant);
        }
    }

   private:
    const TenantFactory factory_;
    // Shared by lookups and walks, exclusive for the rare creates.
    mutable std::shared_mutex mutex_;
    // A node map, so a tenant and its id stay put while others are created.
    std::unordered_map<TenantId, std::unique_ptr<Tenant>, TenantIdHash>
        tenants_;
    // The default tenant once created: a lookup of it skips the lock.
    std::atomic<Tenant*> default_{nullptr};
};

}  // namespace metadata
}  // namespace mooncake
