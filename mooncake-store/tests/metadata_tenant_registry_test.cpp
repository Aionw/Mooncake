#include "metadata/tenant_registry.h"
#include "object_test_helpers.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace mooncake {
namespace metadata {
namespace {

// Every tenant of one registry is built through the factory it was constructed
// with, so its tests can pass a plain one.
std::unique_ptr<Tenant> MakeTenant(const TenantId&) {
    return std::make_unique<Tenant>();
}

class TenantRegistryCreationTest : public ::testing::TestWithParam<TenantId> {};

TEST_P(TenantRegistryCreationTest, GetOrCreateTenantBuildsOncePerTenantId) {
    size_t builds = 0;
    TenantRegistry registry([&builds](const TenantId&) {
        ++builds;
        return std::make_unique<Tenant>();
    });
    const TenantId& tenant = GetParam();
    EXPECT_EQ(registry.Lookup(tenant), nullptr);

    Tenant& created = registry.GetOrCreateTenant(tenant);
    EXPECT_EQ(builds, 1u);
    EXPECT_EQ(registry.Lookup(tenant), &created);

    // A second call finds the published tenant instead of building another.
    EXPECT_EQ(&registry.GetOrCreateTenant(tenant), &created);
    EXPECT_EQ(builds, 1u);
}

TEST_P(TenantRegistryCreationTest,
       ConcurrentCreationPublishesOneWinningTenant) {
    std::atomic<size_t> builds{0};
    TenantRegistry registry([&builds](const TenantId&) {
        builds.fetch_add(1, std::memory_order_relaxed);
        return std::make_unique<Tenant>();
    });
    const TenantId& tenant = GetParam();

    constexpr int kRacers = 16;
    std::vector<Tenant*> observed(kRacers);
    std::atomic<size_t> ready{0};
    std::atomic<bool> start{false};
    std::vector<std::thread> threads;
    threads.reserve(kRacers);
    for (int i = 0; i < kRacers; ++i) {
        threads.emplace_back([&, i] {
            ready.fetch_add(1, std::memory_order_acq_rel);
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            observed[i] = &registry.GetOrCreateTenant(tenant);
        });
    }
    while (ready.load(std::memory_order_acquire) < kRacers) {
        std::this_thread::yield();
    }
    start.store(true, std::memory_order_release);
    for (auto& thread : threads) {
        thread.join();
    }

    for (int i = 0; i < kRacers; ++i) {
        EXPECT_EQ(observed[i], observed[0])
            << "racer " << i << " kept a losing build";
    }
    EXPECT_EQ(registry.Lookup(tenant), observed[0]);
    // Racers that missed the lookup may each build, but only one build is
    // published and every racer holds it.
    EXPECT_GE(builds.load(std::memory_order_relaxed), 1u);
    EXPECT_LE(builds.load(std::memory_order_relaxed),
              static_cast<size_t>(kRacers));
}

// The default tenant resolves without the map lock, a named one through it;
// both must behave the same.
INSTANTIATE_TEST_SUITE_P(DefaultAndNamed, TenantRegistryCreationTest,
                         ::testing::Values(TenantId::Default(),
                                           TenantId("tenant-a")));

TEST(TenantRegistryTest, ClearDropsEveryTenant) {
    TenantRegistry registry(MakeTenant);
    Tenant& old_default = registry.GetOrCreateTenant(TenantId::Default());
    ASSERT_NE(test::PublishObject(old_default.objects, "k1"), 0u);
    (void)registry.GetOrCreateTenant(TenantId("tenant-a"));

    registry.Clear();
    EXPECT_EQ(registry.Lookup(TenantId::Default()), nullptr);
    EXPECT_EQ(registry.Lookup(TenantId("tenant-a")), nullptr);
    size_t visited = 0;
    registry.Visit([&](const TenantId&, Tenant&) { ++visited; });
    EXPECT_EQ(visited, 0u);

    // The next create builds afresh, and the default is found again.
    Tenant& new_default = registry.GetOrCreateTenant(TenantId::Default());
    EXPECT_TRUE(new_default.Empty());
    EXPECT_EQ(registry.Lookup(TenantId::Default()), &new_default);
}

TEST(TenantRegistryTest, VisitReachesEveryTenantAndCarriesABroadcast) {
    TenantRegistry registry(MakeTenant);
    std::vector<Tenant*> tenants;
    for (const TenantId& id : {TenantId::Default(), TenantId("tenant-a")}) {
        tenants.push_back(&registry.GetOrCreateTenant(id));
    }
    for (Tenant* tenant : tenants) {
        ASSERT_NE(test::PublishObject(tenant->objects, "k1", "g1"), 0u);
        ASSERT_NE(test::PublishObject(tenant->objects, "k2", "g1"), 0u);
        // A restored tenant starts without membership.
        test::ObjectRouteTestPeer::DropGroupMemberships(tenant->objects);
        ASSERT_TRUE(tenant->objects.GroupMembers("g1").empty());
    }

    // The broadcast a snapshot restore needs: one walk reaches every tenant,
    // the default one included.
    std::vector<Tenant*> visited;
    registry.Visit([&](const TenantId& tenant_id, Tenant& tenant) {
        EXPECT_EQ(registry.Lookup(tenant_id), &tenant) << tenant_id.value();
        visited.push_back(&tenant);
        tenant.objects.RebuildGroupState();
    });

    std::sort(visited.begin(), visited.end());
    std::sort(tenants.begin(), tenants.end());
    EXPECT_EQ(visited, tenants);
    for (const Tenant* tenant : tenants) {
        EXPECT_EQ(tenant->objects.GroupMembers("g1").size(), 2u);
    }
}

TEST(TenantRegistryTest, VisitWalksTheTenantsPresentWhenItStarted) {
    TenantRegistry registry(MakeTenant);
    (void)registry.GetOrCreateTenant(TenantId("tenant-a"));
    (void)registry.GetOrCreateTenant(TenantId("tenant-b"));

    // Creating a tenant from inside the walk is allowed and must not change
    // what that walk sees: it iterates the tenants present when it started.
    std::vector<std::string> seen;
    registry.Visit([&](const TenantId& tenant_id, Tenant&) {
        seen.push_back(tenant_id.value());
        (void)registry.GetOrCreateTenant(TenantId("tenant-c"));
    });
    EXPECT_EQ(seen.size(), 2u);
    EXPECT_EQ(std::find(seen.begin(), seen.end(), "tenant-c"), seen.end());

    // The next walk sees it.
    std::vector<std::string> after;
    registry.Visit([&](const TenantId& tenant_id, Tenant&) {
        after.push_back(tenant_id.value());
    });
    EXPECT_EQ(after.size(), 3u);
    EXPECT_NE(std::find(after.begin(), after.end(), "tenant-c"), after.end());
}

TEST(TenantRegistryTest, ConcurrentLookupCreateAndVisitStayConsistent) {
    // Every tenant this registry builds arrives with one object already
    // inserted, so a reader that finds a tenant without it saw the tenant
    // published before it had finished being built.
    TenantRegistry registry([](const TenantId&) {
        auto tenant = std::make_unique<Tenant>();
        [[maybe_unused]] const route::Generation generation =
            test::PublishObject(tenant->objects, "k1");
        assert(generation != 0);
        return tenant;
    });
    const std::vector<TenantId> ids = {
        TenantId::Default(), TenantId("tenant-a"), TenantId("tenant-b"),
        TenantId("tenant-c")};
    constexpr uint64_t kRounds = 20000;

    // Once found, a tenant stays where it was: every thread must keep finding
    // the address first published for its id.
    std::array<std::atomic<Tenant*>, 4> published{};
    std::atomic<int> violations{0};
    const auto check = [&](size_t index, const Tenant* tenant) {
        if (tenant == nullptr) {
            return;
        }
        Tenant* expected = nullptr;
        if (!published[index].compare_exchange_strong(
                expected, const_cast<Tenant*>(tenant)) &&
            expected != tenant) {
            violations.fetch_add(1, std::memory_order_relaxed);
        }
        if (tenant->objects.ObjectCount() != 1) {
            violations.fetch_add(1, std::memory_order_relaxed);
        }
    };

    std::vector<std::thread> threads;
    for (int c = 0; c < 2; ++c) {
        threads.emplace_back([&, c] {
            for (uint64_t i = c; i < kRounds; ++i) {
                const size_t index = i % ids.size();
                check(index, &registry.GetOrCreateTenant(ids[index]));
            }
        });
    }
    for (int l = 0; l < 4; ++l) {
        threads.emplace_back([&, l] {
            for (uint64_t i = l; i < kRounds; ++i) {
                const size_t index = i % ids.size();
                check(index, registry.Lookup(ids[index]));
            }
        });
    }
    // A walker creates tenants from inside its own callback, which must
    // neither deadlock nor disturb the walk it is part of.
    threads.emplace_back([&] {
        for (uint64_t walk = 0; walk < kRounds / 10; ++walk) {
            std::vector<std::string> seen;
            registry.Visit([&](const TenantId& tenant_id, Tenant& tenant) {
                const size_t index =
                    std::find(ids.begin(), ids.end(), tenant_id) - ids.begin();
                if (std::find(seen.begin(), seen.end(), tenant_id.value()) !=
                    seen.end()) {
                    violations.fetch_add(1, std::memory_order_relaxed);
                }
                seen.push_back(tenant_id.value());
                check(index, &tenant);
                (void)registry.GetOrCreateTenant(ids[walk % ids.size()]);
            });
        }
    });
    for (auto& thread : threads) {
        thread.join();
    }

    EXPECT_EQ(violations.load(), 0);
    for (size_t index = 0; index < ids.size(); ++index) {
        EXPECT_EQ(registry.Lookup(ids[index]), published[index].load())
            << ids[index].value();
    }
}

}  // namespace
}  // namespace metadata
}  // namespace mooncake
