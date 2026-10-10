#pragma once

// ObjectRoute: one keyspace's key -> object route, its group index, and the
// lock that guards each key.
//
// The lock belongs to the key, not to an object. Every key the route knows has
// one KeySlot: a lock and, while the key holds an object, that object's record.
// A slot outlives the objects published in it, so a writer can lock a key that
// holds no object yet, replace an object without a window in which the key is
// absent, and tear one down by clearing the record. While a slot exists it is
// the only one for its key, so holding its lock excludes every other access to
// the key, the record and its group membership.
//
// Each publication gets a Generation, unique within the route, so code that
// lets go of a key and comes back later (a scan that evicts afterwards, a
// durable callback) names the object it judged with an ObjectRef and acts only
// while that same publication is still there.
//
// Locking: a stripe lock is held only to find, create or collect a slot, and
// across a cursor, which only tries slot locks while it holds one. Nothing
// waits for a slot lock while holding a stripe lock, so a slot holder may
// still look up other keys. A thread holds at most one slot lock at a time (a
// cursor holds the one it stands on), since two writers each holding one key
// and waiting for the other's would deadlock. The group index is a leaf under
// a slot lock.
//
// Layout: every key costs one allocation. Its slot lives in its stripe's map
// node, next to the key it is filed under, and holds the object inline. Guards
// and cursors keep a slot in the map with a SlotHandle; a slot that holds no
// object leaves the map once no handle holds it.

#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include <boost/intrusive/list.hpp>

#include "common/heap_optional.h"
#include "common/transparent_string_hash.h"
#include "metadata/group_index.h"
#include "object_metadata.h"
#include "object_runtime_state.h"

namespace mooncake {

// How a key is locked: shared to read it, exclusive to write it.
enum class LockMode { kRead, kWrite };

namespace test {
struct ObjectRouteTestPeer;
}  // namespace test

namespace route {

// Names one publication of a key within a route. Never reused, never
// persisted: a restore assigns fresh ones.
using Generation = uint64_t;

// A publication, named so it can be found again after its lock is let go.
struct ObjectRef {
    std::string key;
    Generation generation{0};
};

// The per-object runtime state the route keeps next to the metadata: the task
// in flight for the key, at most one of each kind, and the bookkeeping of the
// subsystems that act on it.
//
// Every key carries one, and almost every key carries no task, so each task
// lives on the heap and costs a pointer while absent.
struct ObjectState {
    // A primary write or a background task is in flight for this key.
    bool is_processing{false};
    HeapOptional<ReplicationTask> replication_task;
    HeapOptional<OffloadingTask> offloading_task;
    HeapOptional<PromotionTask> promotion_task;
    HeapOptional<PromotionCandidate> promotion_candidate;
    HeapOptional<DynamicReplicaPending> dynamic_replication_pending;
    std::chrono::steady_clock::time_point dynamic_replication_cooldown{};

    // True while a primary write or a replication, offloading or promotion
    // task is in flight: the work an expiry sweep reclaims.
    [[nodiscard]] bool HasInFlightWork() const noexcept {
        return is_processing || replication_task.has_value() ||
               offloading_task.has_value() || promotion_task.has_value();
    }
};

// What the route's owner keeps on every slot, guarded by the slot lock. The
// route stores it and never reads it; it survives the objects published in the
// slot.
struct SlotOwnerState {
    // Whether the owner's in-flight list holds the slot.
    bool in_flight_listed{false};
    // Whether the owner's eviction index files the slot, and at what rank.
    bool eviction_filed{false};
    int64_t eviction_rank{0};
    // Two words for the owner's namespace policy.
    std::array<uint64_t, 2> policy_words{};
};

// A slot's lock as a guard or cursor holds it in `kMode`.
template <LockMode kMode>
using SlotLock = std::conditional_t<kMode == LockMode::kWrite,
                                    std::unique_lock<std::shared_mutex>,
                                    std::shared_lock<std::shared_mutex>>;

class ObjectRoute;
template <LockMode kMode>
class Guard;
using ReadGuard = Guard<LockMode::kRead>;
using WriteGuard = Guard<LockMode::kWrite>;

// One key's lock and, while the key holds an object, its record. A slot lives
// in its stripe's map node and never moves. Only the route and its guards
// reach inside.
class KeySlot {
   public:
    KeySlot() = default;
    KeySlot(const KeySlot&) = delete;
    KeySlot& operator=(const KeySlot&) = delete;

    const std::string& key() const noexcept { return *key_; }

   private:
    friend class ObjectRoute;
    friend class SlotHandle;
    template <LockMode>
    friend class Guard;
    friend struct test::ObjectRouteTestPeer;

    // Links the slot into one of its owner's lists. Neither the hook nor the
    // list synchronizes: whatever guards a list guards every hook in it. A
    // safe-link hook asserts it is unlinked when destroyed, and a list unlinks
    // whatever it still holds when it is cleared or destroyed.
    using ListHook = boost::intrusive::list_member_hook<
        boost::intrusive::link_mode<boost::intrusive::safe_link>>;

    // One publication. The metadata is built in place, since it can be
    // neither copied nor moved.
    struct Record {
        template <typename... Args>
        explicit Record(Generation publication, Args&&... metadata_args)
            : generation(publication),
              metadata(std::forward<Args>(metadata_args)...) {}

        Generation generation;
        ObjectMetadata metadata;
        ObjectState state;
    };

    // The key the map files this slot under, in the same node.
    const std::string* key_ = nullptr;
    // How many SlotHandles hold the slot. The map's own entry is not one.
    std::atomic<uint32_t> holders_{0};
    mutable std::shared_mutex mutex_;
    std::optional<Record> record_;
    SlotOwnerState owner_;
    // Hooks into the owner's in-flight list and its eviction index.
    ListHook in_flight_hook_;
    ListHook eviction_hook_;

   public:
    // The owner's lists of slots. Each names its hook here, where the hook is
    // accessible, so the slot needs no friend for them.
    using InFlightList = boost::intrusive::list<
        KeySlot, boost::intrusive::member_hook<KeySlot, ListHook,
                                               &KeySlot::in_flight_hook_>>;
    using EvictionList = boost::intrusive::list<
        KeySlot, boost::intrusive::member_hook<KeySlot, ListHook,
                                               &KeySlot::eviction_hook_>>;
};

// A counted hold on a slot. A slot that holds no object stays in the route's
// map while any handle holds it, so a holder can still lock it. A handle is
// only made under the slot's stripe lock, so a collector holding that lock
// exclusively knows no new holder can appear. The route owns its slots, so a
// handle must not outlive it.
class SlotHandle {
   public:
    SlotHandle() = default;
    explicit SlotHandle(KeySlot& slot) noexcept : slot_(&slot) {
        slot.holders_.fetch_add(1, std::memory_order_relaxed);
    }
    SlotHandle(SlotHandle&& other) noexcept
        : slot_(std::exchange(other.slot_, nullptr)) {}
    SlotHandle(const SlotHandle&) = delete;
    SlotHandle& operator=(const SlotHandle&) = delete;
    SlotHandle& operator=(SlotHandle&&) = delete;
    ~SlotHandle() { reset(); }

    KeySlot* get() const noexcept { return slot_; }
    KeySlot& operator*() const noexcept { return *slot_; }
    KeySlot* operator->() const noexcept { return slot_; }
    explicit operator bool() const noexcept { return slot_ != nullptr; }

    // Lets go of the slot. A collector that then finds no holder sees every
    // access this holder made.
    void reset() noexcept {
        if (slot_ != nullptr) {
            slot_->holders_.fetch_sub(1, std::memory_order_release);
            slot_ = nullptr;
        }
    }

   private:
    KeySlot* slot_ = nullptr;
};

// Called by the route as a guard it handed out is released, with the slot
// locked: what the owner keeps about the key is settled there, so the code
// that changed the key need not do it. Read releases are reported only to a
// route built to report them, since every read pays for the call.
class RouteObserver {
   public:
    virtual void OnWriteRelease(const WriteGuard& guard) = 0;
    virtual void OnReadRelease(const ReadGuard& /*guard*/) {}

   protected:
    ~RouteObserver() = default;
};

// Holds one key, read or write, for the caller's scope. Only the route makes
// one. A write guard may hold a key with no object (see
// ObjectRoute::WriteOrCreate); a read guard always holds an object.
//
// A guard holds its slot with a SlotHandle, so the slot outlives its lock. It
// moves but does not assign, since assigning would drop one key's lock while
// taking another's.
template <LockMode kMode>
class Guard {
    static constexpr bool kWrite = kMode == LockMode::kWrite;
    using Lock = SlotLock<kMode>;

   public:
    using Metadata =
        std::conditional_t<kWrite, ObjectMetadata, const ObjectMetadata>;
    using State = std::conditional_t<kWrite, ObjectState, const ObjectState>;

    Guard(Guard&& other) noexcept
        : route_(std::exchange(other.route_, nullptr)),
          slot_(std::move(other.slot_)),
          lock_(std::move(other.lock_)),
          handed_out_(other.handed_out_) {}
    Guard(const Guard&) = delete;
    Guard& operator=(const Guard&) = delete;
    Guard& operator=(Guard&&) = delete;
    ~Guard();

    const std::string& key() const { return slot_->key(); }

    // Whether the key holds an object.
    bool has_object() const { return slot_->record_.has_value(); }

    // The rest require has_object().
    Generation generation() const { return record().generation; }
    ObjectRef ref() const { return {key(), generation()}; }
    Metadata& metadata() const { return record().metadata; }
    State& state() const { return record().state; }

    // Publishes an object under this key, which holds none: builds its
    // metadata in place from `metadata_args`, ObjectMetadata's constructor
    // arguments, and wires its group membership and the group's shared lease;
    // the state starts empty. Returns the new publication's generation.
    //
    // Nobody else can reach the object until the guard is released, so the
    // caller finishes setting it up through metadata() and state().
    template <typename... Args>
    Generation Publish(Args&&... metadata_args) const
        requires kWrite;

    // Ends the publication this guard holds: drops its group membership and
    // the record. The caller gives back whatever hangs off the object first,
    // under this same guard. A later Publish on the guard starts a new
    // publication of the key with nothing in between.
    void TearDown() const
        requires kWrite;

    // The owner's per-slot state, for the route's owner only.
    SlotOwnerState& owner_state() const
        requires kWrite
    {
        return slot_->owner_;
    }
    KeySlot& slot() const { return *slot_; }

   private:
    friend class ObjectRoute;

    Guard(ObjectRoute* route, SlotHandle slot) NO_THREAD_SAFETY_ANALYSIS
        : route_(route),
          slot_(std::move(slot)),
          lock_(slot_->mutex_) {}

    // Marks the guard as handed to a caller, so its release is reported to
    // the observer; one the route takes and drops itself, finding the key
    // empty or on another publication, is not.
    Guard&& HandOut() && {
        handed_out_ = true;
        return std::move(*this);
    }

    KeySlot::Record& record() const {
        assert(slot_->record_.has_value());
        return *slot_->record_;
    }

    ObjectRoute* route_;
    // Declared before the lock so it is destroyed after it.
    SlotHandle slot_;
    Lock lock_;
    bool handed_out_ = false;
};

class ObjectRoute {
    // Each slot is built in its map node and stays there while the map
    // grows, so a slot's address and its key's are stable.
    using SlotMap = std::unordered_map<std::string, KeySlot,
                                       TransparentStringHash, std::equal_to<>>;

   public:
    // A stripe's map rehashes under its exclusive lock, which stalls every
    // reader of the stripe, and a batch read touches every stripe. Many
    // small stripes keep each stall short.
    static constexpr size_t kStripeCount = 1024;

    // `observer`, when set, outlives the route; it hears of read releases
    // only when `report_reads` is set.
    explicit ObjectRoute(RouteObserver* observer = nullptr,
                         bool report_reads = false)
        : observer_(observer),
          report_reads_(observer != nullptr && report_reads) {}
    ObjectRoute(const ObjectRoute&) = delete;
    ObjectRoute& operator=(const ObjectRoute&) = delete;

    // The object under `key`, held shared; nullopt when the key holds none.
    [[nodiscard]] std::optional<ReadGuard> Read(std::string_view key) const {
        return Acquire<LockMode::kRead>(key, std::nullopt);
    }
    // The same, only while `ref` is still the key's current publication.
    [[nodiscard]] std::optional<ReadGuard> Read(const ObjectRef& ref) const {
        return Acquire<LockMode::kRead>(ref.key, ref.generation);
    }

    // The object under `key`, held exclusively; nullopt when it holds none.
    [[nodiscard]] std::optional<WriteGuard> Write(std::string_view key) {
        return Acquire<LockMode::kWrite>(key, std::nullopt);
    }
    // The same, only while `ref` is still the key's current publication.
    [[nodiscard]] std::optional<WriteGuard> Write(const ObjectRef& ref) {
        return Acquire<LockMode::kWrite>(ref.key, ref.generation);
    }
    // The key held exclusively whether or not it holds an object, so the
    // caller can publish one.
    [[nodiscard]] WriteGuard WriteOrCreate(std::string_view key);

    // Whether `key` holds an object right now. It takes the key's lock, so a
    // caller already holding that key must not ask.
    [[nodiscard]] bool Contains(std::string_view key) const {
        return Read(key).has_value();
    }

    [[nodiscard]] size_t ObjectCount() const {
        return object_count_.load(std::memory_order_relaxed);
    }

    // True when the route holds no object and no group membership.
    [[nodiscard]] bool Empty() const {
        return ObjectCount() == 0 && groups_.Empty();
    }

    template <LockMode kMode>
    class Cursor;

    // A cursor over every object, each visited under its own key's read or
    // write lock:
    //
    //     for (auto object : route.ReadCursor()) { ... object.metadata() ... }
    //
    // Each object is visited at most once, and the cursor moves one stripe at
    // a time rather than reading one point in time: an object published or
    // torn down meanwhile may or may not be seen. A write cursor may change an
    // object's metadata and state but not publish or tear down, and it does
    // not run the observer, so it must not start or finish in-flight work.
    //
    // The cursor holds a stripe lock across the loop body, so the body must
    // not reach this route or another one (debug builds assert it), nor block
    // on a lock whose holder may be waiting on a route. One that acts after
    // the cursor keeps `ref()` and goes through Write(ref).
    [[nodiscard]] Cursor<LockMode::kRead> ReadCursor() const;
    [[nodiscard]] Cursor<LockMode::kWrite> WriteCursor();

    // The member keys of one group, as a snapshot to resolve again: a key in
    // it may hold a different object by the time the caller reads it, or none.
    [[nodiscard]] std::vector<std::string> GroupMembers(
        std::string_view group_id) const {
        return groups_.Members(group_id);
    }

    // Rebuilds group membership and the group leases from object metadata,
    // for the snapshot and standby restore paths. The first pass takes the
    // latest restored deadline per group, so a grouped object is not left on a
    // zero-deadline lease that post-restore cleanup would drop; the second
    // registers membership and points every grouped object at its group's
    // shared lease.
    void RebuildGroupState();

    // Drops slots that hold no object and that nobody references. A slot is
    // collected as soon as its last writer leaves it empty, unless another
    // caller still held it then; this sweeps those up, visiting only the
    // stripes where that happened. Returns how many went.
    size_t SweepEmptySlots();

   private:
    template <LockMode>
    friend class Guard;
    friend struct test::ObjectRouteTestPeer;

    struct Stripe {
        mutable std::shared_mutex lock;
        SlotMap slots;
        // Empty slots Collect had to leave behind because another caller
        // still held them; SweepEmptySlots visits the stripe while non-zero.
        std::atomic<size_t> uncollected{0};
    };

    [[nodiscard]] static size_t StripeIndex(std::string_view key) {
        return TransparentStringHash{}(key) % kStripeCount;
    }
    [[nodiscard]] Stripe& StripeOf(std::string_view key) const {
        return stripes_[StripeIndex(key)];
    }

    // The key locked in `kMode`, handed out only while it holds an object
    // and, given a `generation`, only while that is the current publication.
    template <LockMode kMode>
    [[nodiscard]] std::optional<Guard<kMode>> Acquire(
        std::string_view key, std::optional<Generation> generation) const;

    // A handle on `key`'s slot; empty when the route has none.
    [[nodiscard]] SlotHandle FindSlot(std::string_view key) const;
    [[nodiscard]] SlotHandle FindOrCreateSlot(std::string_view key);
    // Drops the slot `handle` holds from the map when it holds no object and
    // `handle` is its only holder. Called with no slot lock held.
    void Collect(SlotHandle handle);

    template <typename... Args>
    Generation PublishLocked(KeySlot& slot, Args&&... metadata_args);
    void TearDownLocked(KeySlot& slot);

    // How many stripe locks this thread holds for a cursor. A route access
    // made while one is held is the cursor's loop body reaching a route, which
    // can deadlock against a writer that holds a slot and waits on a stripe.
    static inline thread_local int cursor_stripes_ = 0;
    static void AssertNotInCursor() {
        assert(cursor_stripes_ == 0 &&
               "route access from inside an object cursor");
    }

    RouteObserver* const observer_;
    const bool report_reads_;
    std::atomic<Generation> next_generation_{1};
    std::atomic<size_t> object_count_{0};
    mutable std::array<Stripe, kStripeCount> stripes_;
    // Group membership and the one shared Lease per group.
    GroupIndex groups_;

   public:
    // The single-pass cursor behind ReadCursor() and WriteCursor(). It owns
    // the locks of the position it stands on, so it neither copies nor moves,
    // and leaving the loop early releases them.
    //
    // While it holds a stripe the cursor only tries a slot's lock: a slot
    // busy elsewhere is set aside and visited after the stripes, under a lock
    // it waits for.
    template <LockMode kMode>
    class Cursor {
        static constexpr bool kWrite = kMode == LockMode::kWrite;
        using Lock = SlotLock<kMode>;

       public:
        // The object a position stands on, valid until the cursor advances.
        class Object {
           public:
            using Metadata = typename Guard<kMode>::Metadata;
            using State = typename Guard<kMode>::State;

            const std::string& key() const { return slot_.key(); }
            Generation generation() const { return slot_.record_->generation; }
            ObjectRef ref() const { return {key(), generation()}; }
            Metadata& metadata() const { return slot_.record_->metadata; }
            State& state() const { return slot_.record_->state; }
            // The owner's per-slot state, for the route's owner only.
            SlotOwnerState& owner_state() const
                requires kWrite
            {
                return slot_.owner_;
            }

           private:
            friend class Cursor;
            explicit Object(KeySlot& slot) : slot_(slot) {}
            KeySlot& slot_;
        };

        struct Sentinel {};

        class Iterator {
           public:
            Object operator*() const { return Object(*cursor_->current_); }
            Iterator& operator++() {
                cursor_->Advance();
                return *this;
            }
            bool operator!=(Sentinel) const {
                return cursor_->current_ != nullptr;
            }

           private:
            friend class Cursor;
            explicit Iterator(Cursor* cursor) : cursor_(cursor) {}
            Cursor* cursor_;
        };

        Cursor(const Cursor&) = delete;
        Cursor& operator=(const Cursor&) = delete;
        ~Cursor() {
            slot_lock_ = Lock();
            ReleaseStripe();
        }

        Iterator begin() { return Iterator(this); }
        Sentinel end() const { return {}; }

       private:
        friend class ObjectRoute;

        explicit Cursor(const ObjectRoute& route) : route_(route) { Settle(); }

        void Advance() {
            // A write cursor does not run the observer, so the loop body must
            // leave the object's in-flight work as it found it.
            assert(!kWrite || current_ == nullptr ||
                   current_->record_->state.HasInFlightWork() ==
                       visited_in_flight_);
            slot_lock_ = Lock();
            if (stripe_ < kStripeCount) {
                ++slot_it_;
            } else {
                ++deferred_pos_;
            }
            Settle();
        }

        // Moves to the first object at or after the current position whose
        // lock it obtains, or to the end.
        void Settle() NO_THREAD_SAFETY_ANALYSIS {
            for (; stripe_ < kStripeCount; ++stripe_) {
                Stripe& stripe = route_.stripes_[stripe_];
                if (!stripe_lock_.owns_lock()) {
                    AssertNotInCursor();
                    stripe_lock_ =
                        std::shared_lock<std::shared_mutex>(stripe.lock);
                    ++cursor_stripes_;
                    slot_it_ = stripe.slots.begin();
                }
                for (; slot_it_ != stripe.slots.end(); ++slot_it_) {
                    KeySlot& slot = slot_it_->second;
                    Lock lock(slot.mutex_, std::try_to_lock);
                    if (!lock.owns_lock()) {
                        deferred_.emplace_back(slot);
                        continue;
                    }
                    if (slot.record_.has_value()) {
                        Stand(slot, std::move(lock));
                        return;
                    }
                }
                ReleaseStripe();
            }
            for (; deferred_pos_ < deferred_.size(); ++deferred_pos_) {
                KeySlot& slot = *deferred_[deferred_pos_];
                Lock lock(slot.mutex_);
                if (slot.record_.has_value()) {
                    Stand(slot, std::move(lock));
                    return;
                }
            }
            current_ = nullptr;
        }

        void Stand(KeySlot& slot, Lock lock) {
            slot_lock_ = std::move(lock);
            current_ = &slot;
            visited_in_flight_ = slot.record_->state.HasInFlightWork();
        }

        void ReleaseStripe() {
            if (stripe_lock_.owns_lock()) {
                stripe_lock_.unlock();
                --cursor_stripes_;
            }
        }

        const ObjectRoute& route_;
        size_t stripe_ = 0;
        std::shared_lock<std::shared_mutex> stripe_lock_;
        typename SlotMap::iterator slot_it_;
        // Slots that were busy when the cursor passed their stripe; the
        // handles keep them in the map until they are visited.
        std::vector<SlotHandle> deferred_;
        size_t deferred_pos_ = 0;
        Lock slot_lock_;
        KeySlot* current_ = nullptr;
        // Whether the object stood on carried in-flight work when reached.
        bool visited_in_flight_ = false;
    };
};

// --- Guard ------------------------------------------------------------------

template <LockMode kMode>
Guard<kMode>::~Guard() {
    if (route_ == nullptr) {
        return;  // moved from
    }
    if constexpr (kWrite) {
        if (handed_out_ && route_->observer_ != nullptr) {
            route_->observer_->OnWriteRelease(*this);
        }
    } else {
        if (handed_out_ && route_->report_reads_) {
            route_->observer_->OnReadRelease(*this);
        }
    }
    const bool empty = !slot_->record_.has_value();
    lock_.unlock();
    if (empty) {
        route_->Collect(std::move(slot_));
    }
}

template <LockMode kMode>
template <typename... Args>
Generation Guard<kMode>::Publish(Args&&... metadata_args) const
    requires kWrite
{
    assert(!has_object());
    return route_->PublishLocked(*slot_, std::forward<Args>(metadata_args)...);
}

template <LockMode kMode>
void Guard<kMode>::TearDown() const
    requires kWrite
{
    assert(has_object());
    route_->TearDownLocked(*slot_);
}

// --- ObjectRoute ------------------------------------------------------------

inline SlotHandle ObjectRoute::FindSlot(std::string_view key) const {
    AssertNotInCursor();
    Stripe& stripe = StripeOf(key);
    std::shared_lock<std::shared_mutex> lock(stripe.lock);
    const auto it = stripe.slots.find(key);
    return it == stripe.slots.end() ? SlotHandle() : SlotHandle(it->second);
}

inline SlotHandle ObjectRoute::FindOrCreateSlot(std::string_view key) {
    if (SlotHandle slot = FindSlot(key)) {
        return slot;
    }
    Stripe& stripe = StripeOf(key);
    std::unique_lock<std::shared_mutex> lock(stripe.lock);
    const auto [it, created] = stripe.slots.try_emplace(std::string(key));
    if (created) {
        it->second.key_ = &it->first;
    }
    return SlotHandle(it->second);
}

inline void ObjectRoute::Collect(SlotHandle handle) {
    Stripe& stripe = StripeOf(handle->key());
    std::unique_lock<std::shared_mutex> lock(stripe.lock);
    // A handle is only made under the stripe lock, so with ours the only one,
    // nobody else can reach the slot. Locking it then never waits, and it
    // orders this read of the record after the last writer's release.
    if (handle->holders_.load(std::memory_order_acquire) != 1) {
        stripe.uncollected.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    std::unique_lock<std::shared_mutex> slot_lock(handle->mutex_,
                                                  std::try_to_lock);
    if (!slot_lock.owns_lock() || handle->record_.has_value()) {
        return;
    }
    // Erasing destroys the slot, so its lock and our hold go first.
    const auto it = stripe.slots.find(handle->key());
    slot_lock.unlock();
    handle.reset();
    stripe.slots.erase(it);
}

template <LockMode kMode>
std::optional<Guard<kMode>> ObjectRoute::Acquire(
    std::string_view key, std::optional<Generation> generation) const {
    SlotHandle slot = FindSlot(key);
    if (!slot) {
        return std::nullopt;
    }
    Guard<kMode> guard(const_cast<ObjectRoute*>(this), std::move(slot));
    if (!guard.has_object() ||
        (generation.has_value() && guard.generation() != *generation)) {
        return std::nullopt;
    }
    return std::optional<Guard<kMode>>(std::move(guard).HandOut());
}

inline WriteGuard ObjectRoute::WriteOrCreate(std::string_view key) {
    return WriteGuard(this, FindOrCreateSlot(key)).HandOut();
}

template <typename... Args>
Generation ObjectRoute::PublishLocked(KeySlot& slot, Args&&... metadata_args) {
    const Generation generation =
        next_generation_.fetch_add(1, std::memory_order_relaxed);
    ObjectMetadata& metadata =
        slot.record_.emplace(generation, std::forward<Args>(metadata_args)...)
            .metadata;
    assert(metadata.user_key == slot.key());
    if (!metadata.group_id.empty()) {
        // AddMember returns null only for an empty group_id.
        auto lease = groups_.AddMember(metadata.group_id, slot.key());
        assert(lease != nullptr);
        SpinLocker locker(&metadata.lock);
        metadata.lease_ = std::move(lease);
    }
    object_count_.fetch_add(1, std::memory_order_relaxed);
    return generation;
}

inline void ObjectRoute::TearDownLocked(KeySlot& slot) {
    const std::string& group_id = slot.record_->metadata.group_id;
    if (!group_id.empty()) {
        (void)groups_.RemoveMember(group_id, slot.key());
    }
    slot.record_.reset();
    object_count_.fetch_sub(1, std::memory_order_relaxed);
}

inline ObjectRoute::Cursor<LockMode::kRead> ObjectRoute::ReadCursor() const {
    return Cursor<LockMode::kRead>(*this);
}

inline ObjectRoute::Cursor<LockMode::kWrite> ObjectRoute::WriteCursor() {
    return Cursor<LockMode::kWrite>(*this);
}

inline void ObjectRoute::RebuildGroupState() {
    std::unordered_map<std::string, std::chrono::system_clock::time_point>
        max_deadline_by_group;
    for (auto object : ReadCursor()) {
        const ObjectMetadata& metadata = object.metadata();
        if (!metadata.IsGrouped()) {
            continue;
        }
        const auto deadline = metadata.EvictionDeadline();
        auto [it, inserted] =
            max_deadline_by_group.try_emplace(metadata.group_id, deadline);
        if (!inserted) {
            it->second = std::max(it->second, deadline);
        }
    }
    // The group index is a leaf under a slot lock, so the loop body registers
    // membership itself.
    for (auto object : WriteCursor()) {
        const std::string& group_id = object.metadata().group_id;
        if (group_id.empty()) {
            continue;
        }
        auto lease = groups_.AddMember(group_id, object.key());
        assert(lease != nullptr);
        const auto it = max_deadline_by_group.find(group_id);
        if (it != max_deadline_by_group.end()) {
            lease->ExtendTo(it->second);
        }
        SpinLocker locker(&object.metadata().lock);
        object.metadata().lease_ = std::move(lease);
    }
}

inline size_t ObjectRoute::SweepEmptySlots() {
    AssertNotInCursor();
    size_t swept = 0;
    for (Stripe& stripe : stripes_) {
        if (stripe.uncollected.load(std::memory_order_relaxed) == 0) {
            continue;
        }
        std::unique_lock<std::shared_mutex> lock(stripe.lock);
        stripe.uncollected.store(0, std::memory_order_relaxed);
        for (auto it = stripe.slots.begin(); it != stripe.slots.end();) {
            KeySlot& slot = it->second;
            std::unique_lock<std::shared_mutex> slot_lock(slot.mutex_,
                                                          std::try_to_lock);
            if (!slot_lock.owns_lock() || slot.record_.has_value()) {
                // A holder collects the slot itself if it leaves it empty.
                ++it;
            } else if (slot.holders_.load(std::memory_order_acquire) != 0) {
                // Empty but still held, as by a cursor that set it aside: try
                // again on the next sweep.
                stripe.uncollected.fetch_add(1, std::memory_order_relaxed);
                ++it;
            } else {
                // Nothing holds it, and nobody can make a new handle while the
                // stripe is held.
                slot_lock.unlock();
                it = stripe.slots.erase(it);
                ++swept;
            }
        }
    }
    return swept;
}

}  // namespace route
}  // namespace mooncake
