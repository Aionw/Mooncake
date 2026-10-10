#include "metadata/namespace.h"
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
// namespace adds on top: the in-flight list it keeps from every released write
// guard, and Empty().

std::vector<std::string> SortedInFlightKeys(const Namespace& ns) {
    auto keys = ns.InFlightKeys();
    std::sort(keys.begin(), keys.end());
    return keys;
}

// Starts or finishes a primary write on the object under `key`, under a write
// guard of its own.
void SetProcessing(Namespace& ns, const std::string& key, bool processing) {
    auto guard = ns.objects.Write(key);
    ASSERT_TRUE(guard.has_value());
    guard->state().is_processing = processing;
}

TEST(NamespaceTest, InFlightListsOnlyObjectsWithWork) {
    Namespace ns;
    ASSERT_NE(test::PublishObject(ns.objects, "idle"), 0u);
    ASSERT_NE(test::PublishObject(ns.objects, "busy"), 0u);
    EXPECT_TRUE(ns.InFlightKeys().empty());

    {
        auto guard = ns.objects.Write("busy");
        ASSERT_TRUE(guard.has_value());
        guard->state().is_processing = true;
        // Listed as the guard is released, not before.
        EXPECT_TRUE(ns.InFlightKeys().empty());
    }
    EXPECT_EQ(SortedInFlightKeys(ns), (std::vector<std::string>{"busy"}));

    // A write guard on an object with no work lists nothing.
    {
        auto guard = ns.objects.Write("idle");
        ASSERT_TRUE(guard.has_value());
    }
    // Starting work again on a listed object lists it once.
    SetProcessing(ns, "busy", true);
    EXPECT_EQ(SortedInFlightKeys(ns), (std::vector<std::string>{"busy"}));

    SetProcessing(ns, "busy", false);
    EXPECT_TRUE(ns.InFlightKeys().empty());
}

TEST(NamespaceTest, AKeyStaysListedWhileAnyWorkRemains) {
    Namespace ns;
    ASSERT_NE(test::PublishObject(ns.objects, "k1"), 0u);

    {
        auto guard = ns.objects.Write("k1");
        ASSERT_TRUE(guard.has_value());
        guard->state().is_processing = true;
        guard->state().offloading_task =
            OffloadingTask{1, std::chrono::system_clock::now(), {}};
    }

    // The write finished but the offload has not, so the key stays listed.
    SetProcessing(ns, "k1", false);
    EXPECT_EQ(SortedInFlightKeys(ns), (std::vector<std::string>{"k1"}));

    {
        auto guard = ns.objects.Write("k1");
        ASSERT_TRUE(guard.has_value());
        guard->state().offloading_task.reset();
    }
    EXPECT_TRUE(ns.InFlightKeys().empty());
}

TEST(NamespaceTest, PublishingWithWorkListsTheKeyOnRelease) {
    Namespace ns;
    {
        // The write that creates the object starts with work in flight.
        auto guard = ns.objects.WriteOrCreate("k1");
        ASSERT_FALSE(guard.has_object());
        (void)test::PublishEnvelope(guard);
        guard.state().is_processing = true;
        EXPECT_TRUE(ns.InFlightKeys().empty());
    }
    EXPECT_EQ(SortedInFlightKeys(ns), (std::vector<std::string>{"k1"}));

    // A second writer that finds the key taken publishes nothing and leaves
    // the listing as it is.
    EXPECT_EQ(test::PublishObject(ns.objects, "k1"), 0u);
    EXPECT_EQ(SortedInFlightKeys(ns), (std::vector<std::string>{"k1"}));

    SetProcessing(ns, "k1", false);
    EXPECT_TRUE(ns.InFlightKeys().empty());
}

TEST(NamespaceTest, AWriterThatPublishesNothingListsNothing) {
    Namespace ns;
    {
        auto guard = ns.objects.WriteOrCreate("k1");
    }
    EXPECT_TRUE(ns.InFlightKeys().empty());
    EXPECT_TRUE(ns.Empty());
}

TEST(NamespaceTest, TearDownTakesAKeyOffTheInFlightList) {
    Namespace ns;
    ASSERT_NE(test::PublishObject(ns.objects, "k1"), 0u);
    ASSERT_NE(test::PublishObject(ns.objects, "k2"), 0u);
    SetProcessing(ns, "k1", true);
    SetProcessing(ns, "k2", true);
    ASSERT_EQ(SortedInFlightKeys(ns), (std::vector<std::string>{"k1", "k2"}));

    // The object still carried its write, yet the key leaves the list with
    // it.
    {
        auto guard = ns.objects.Write("k1");
        ASSERT_TRUE(guard.has_value());
        guard->TearDown();
    }
    EXPECT_EQ(SortedInFlightKeys(ns), (std::vector<std::string>{"k2"}));

    SetProcessing(ns, "k2", false);
    EXPECT_TRUE(ns.InFlightKeys().empty());
}

TEST(NamespaceTest, AReplacementIsListedByItsOwnWork) {
    Namespace ns;
    ASSERT_NE(test::PublishObject(ns.objects, "k1"), 0u);
    SetProcessing(ns, "k1", true);

    // A replacement under one guard starts with empty state, so the key is
    // unlisted unless the new publication carries work of its own.
    {
        auto guard = ns.objects.Write("k1");
        ASSERT_TRUE(guard.has_value());
        guard->TearDown();
        (void)test::PublishEnvelope(*guard);
    }
    EXPECT_TRUE(ns.InFlightKeys().empty());

    {
        auto guard = ns.objects.Write("k1");
        ASSERT_TRUE(guard.has_value());
        guard->TearDown();
        (void)test::PublishEnvelope(*guard);
        guard->state().is_processing = true;
    }
    EXPECT_EQ(SortedInFlightKeys(ns), (std::vector<std::string>{"k1"}));

    SetProcessing(ns, "k1", false);
}

TEST(NamespaceTest, DestroyingATenantWithListedKeysIsClean) {
    // The list is declared after the route, so it lets go of the slots' hooks
    // before the route destroys the slots: none is destroyed still linked.
    Namespace ns;
    ASSERT_NE(test::PublishObject(ns.objects, "k1"), 0u);
    ASSERT_NE(test::PublishObject(ns.objects, "k2", "g1"), 0u);
    SetProcessing(ns, "k1", true);
    SetProcessing(ns, "k2", true);
    ASSERT_EQ(ns.InFlightKeys().size(), 2u);
}

TEST(NamespaceTest, EmptyTracksObjectsAndGroups) {
    Namespace ns;
    EXPECT_TRUE(ns.Empty());

    ASSERT_NE(test::PublishObject(ns.objects, "k1", "g1"), 0u);
    EXPECT_FALSE(ns.Empty());

    // Tearing the object down drops its membership with it.
    {
        auto guard = ns.objects.Write("k1");
        ASSERT_TRUE(guard.has_value());
        guard->TearDown();
    }
    EXPECT_TRUE(ns.Empty());
    EXPECT_TRUE(ns.objects.GroupMembers("g1").empty());
}

}  // namespace
}  // namespace metadata
}  // namespace mooncake
