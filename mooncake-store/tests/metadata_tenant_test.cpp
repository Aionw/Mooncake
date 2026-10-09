#include "metadata/tenant.h"
#include "object_test_helpers.h"

#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace mooncake {
namespace metadata {
namespace {

// The route's own behaviour (publication, generations, groups, cursors, slot
// collection) is covered by object_route_test.cpp. These cases cover what the
// tenant adds on top: the in-flight list it keeps from every released write
// guard, and Empty().

std::vector<std::string> SortedInFlightKeys(const Tenant& tenant) {
    auto keys = tenant.InFlightKeys();
    std::sort(keys.begin(), keys.end());
    return keys;
}

// Starts or finishes a primary write on the object under `key`, under a write
// guard of its own.
void SetProcessing(Tenant& tenant, const std::string& key, bool processing) {
    auto guard = tenant.objects.Write(key);
    ASSERT_TRUE(guard.has_value());
    guard->state().is_processing = processing;
}

TEST(TenantTest, InFlightListsOnlyObjectsWithWork) {
    Tenant tenant;
    ASSERT_NE(test::PublishObject(tenant.objects, "idle"), 0u);
    ASSERT_NE(test::PublishObject(tenant.objects, "busy"), 0u);
    EXPECT_TRUE(tenant.InFlightKeys().empty());

    {
        auto guard = tenant.objects.Write("busy");
        ASSERT_TRUE(guard.has_value());
        guard->state().is_processing = true;
        // Listed as the guard is released, not before.
        EXPECT_TRUE(tenant.InFlightKeys().empty());
    }
    EXPECT_EQ(SortedInFlightKeys(tenant), (std::vector<std::string>{"busy"}));

    // A write guard on an object with no work lists nothing.
    {
        auto guard = tenant.objects.Write("idle");
        ASSERT_TRUE(guard.has_value());
    }
    // Starting work again on a listed object lists it once.
    SetProcessing(tenant, "busy", true);
    EXPECT_EQ(SortedInFlightKeys(tenant), (std::vector<std::string>{"busy"}));

    SetProcessing(tenant, "busy", false);
    EXPECT_TRUE(tenant.InFlightKeys().empty());
}

TEST(TenantTest, AKeyStaysListedWhileAnyWorkRemains) {
    Tenant tenant;
    ASSERT_NE(test::PublishObject(tenant.objects, "k1"), 0u);

    {
        auto guard = tenant.objects.Write("k1");
        ASSERT_TRUE(guard.has_value());
        guard->state().is_processing = true;
        guard->state().offloading_task =
            OffloadingTask{1, std::chrono::system_clock::now(), {}};
    }

    // The write finished but the offload has not, so the key stays listed.
    SetProcessing(tenant, "k1", false);
    EXPECT_EQ(SortedInFlightKeys(tenant), (std::vector<std::string>{"k1"}));

    {
        auto guard = tenant.objects.Write("k1");
        ASSERT_TRUE(guard.has_value());
        guard->state().offloading_task.reset();
    }
    EXPECT_TRUE(tenant.InFlightKeys().empty());
}

TEST(TenantTest, PublishingWithWorkListsTheKeyOnRelease) {
    Tenant tenant;
    {
        // The write that creates the object starts with work in flight.
        auto guard = tenant.objects.WriteOrCreate("k1");
        ASSERT_FALSE(guard.has_object());
        (void)guard.Publish(test::MakeObjectMetadata("k1"));
        guard.state().is_processing = true;
        EXPECT_TRUE(tenant.InFlightKeys().empty());
    }
    EXPECT_EQ(SortedInFlightKeys(tenant), (std::vector<std::string>{"k1"}));

    // A second writer that finds the key taken publishes nothing and leaves
    // the listing as it is.
    EXPECT_EQ(test::PublishObject(tenant.objects, "k1"), 0u);
    EXPECT_EQ(SortedInFlightKeys(tenant), (std::vector<std::string>{"k1"}));

    SetProcessing(tenant, "k1", false);
    EXPECT_TRUE(tenant.InFlightKeys().empty());
}

TEST(TenantTest, AWriterThatPublishesNothingListsNothing) {
    Tenant tenant;
    {
        auto guard = tenant.objects.WriteOrCreate("k1");
    }
    EXPECT_TRUE(tenant.InFlightKeys().empty());
    EXPECT_TRUE(tenant.Empty());
}

TEST(TenantTest, TearDownTakesAKeyOffTheInFlightList) {
    Tenant tenant;
    ASSERT_NE(test::PublishObject(tenant.objects, "k1"), 0u);
    ASSERT_NE(test::PublishObject(tenant.objects, "k2"), 0u);
    SetProcessing(tenant, "k1", true);
    SetProcessing(tenant, "k2", true);
    ASSERT_EQ(SortedInFlightKeys(tenant),
              (std::vector<std::string>{"k1", "k2"}));

    // The object still carried its write, yet the key leaves the list with
    // it.
    {
        auto guard = tenant.objects.Write("k1");
        ASSERT_TRUE(guard.has_value());
        guard->TearDown();
    }
    EXPECT_EQ(SortedInFlightKeys(tenant), (std::vector<std::string>{"k2"}));

    SetProcessing(tenant, "k2", false);
    EXPECT_TRUE(tenant.InFlightKeys().empty());
}

TEST(TenantTest, AReplacementIsListedByItsOwnWork) {
    Tenant tenant;
    ASSERT_NE(test::PublishObject(tenant.objects, "k1"), 0u);
    SetProcessing(tenant, "k1", true);

    // A replacement under one guard starts with empty state, so the key is
    // unlisted unless the new publication carries work of its own.
    {
        auto guard = tenant.objects.Write("k1");
        ASSERT_TRUE(guard.has_value());
        guard->TearDown();
        (void)guard->Publish(test::MakeObjectMetadata("k1"));
    }
    EXPECT_TRUE(tenant.InFlightKeys().empty());

    {
        auto guard = tenant.objects.Write("k1");
        ASSERT_TRUE(guard.has_value());
        guard->TearDown();
        (void)guard->Publish(test::MakeObjectMetadata("k1"));
        guard->state().is_processing = true;
    }
    EXPECT_EQ(SortedInFlightKeys(tenant), (std::vector<std::string>{"k1"}));

    SetProcessing(tenant, "k1", false);
}

TEST(TenantTest, DestroyingATenantWithListedKeysIsClean) {
    // The list is declared after the route, so it lets go of the slots' hooks
    // before the route destroys the slots: none is destroyed still linked.
    Tenant tenant;
    ASSERT_NE(test::PublishObject(tenant.objects, "k1"), 0u);
    ASSERT_NE(test::PublishObject(tenant.objects, "k2", "g1"), 0u);
    SetProcessing(tenant, "k1", true);
    SetProcessing(tenant, "k2", true);
    ASSERT_EQ(tenant.InFlightKeys().size(), 2u);
}

TEST(TenantTest, EmptyTracksObjectsAndGroups) {
    Tenant tenant;
    EXPECT_TRUE(tenant.Empty());

    ASSERT_NE(test::PublishObject(tenant.objects, "k1", "g1"), 0u);
    EXPECT_FALSE(tenant.Empty());

    // Tearing the object down drops its membership with it.
    {
        auto guard = tenant.objects.Write("k1");
        ASSERT_TRUE(guard.has_value());
        guard->TearDown();
    }
    EXPECT_TRUE(tenant.Empty());
    EXPECT_TRUE(tenant.objects.GroupMembers("g1").empty());
}

}  // namespace
}  // namespace metadata
}  // namespace mooncake
