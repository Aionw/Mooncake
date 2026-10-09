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

#include "common/intrusive_list.h"
#include "common/transparent_string_hash.h"
#include "group_index.h"
#include "object_metadata.h"
#include "object_runtime_state.h"

namespace mooncake {

// How a key is locked: shared to read it, exclusive to write it.
enum class LockMode { kRead, kWrite };

// Tags the hook that links a slot into its owner's in-flight list.
struct InFlightListTag;

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
struct ObjectState {
    // A primary write or a background task is in flight for this key.
    bool is_processing{false};
    std::optional<ReplicationTask> replication_task;
    std::optional<OffloadingTask> offloading_task;
    std::optional<PromotionTask> promotion_task;
    std::optional<PromotionCandidate> promotion_candidate;
    std::optional<DynamicReplicaPending> dynamic_replication_pending;
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
    // Two words for the owner's namespace policy.
    std::array<uint64_t, 2> policy_words{};
};

class ObjectRoute;
template <LockMode kMode>
class Guard;
using ReadGuard = Guard<LockMode::kRead>;
using WriteGuard = Guard<LockMode::kWrite>;

// One key's lock and, while the key holds an object, its record. Only the
// route and its guards reach inside.
class KeySlot : private IntrusiveListHook<InFlightListTag> {
   public:
    explicit KeySlot(std::string key) : key_(std::move(key)) {}
    KeySlot(const KeySlot&) = delete;
    KeySlot& operator=(const KeySlot&) = delete;

    const std::string& key() const noexcept { return key_; }

   private:
    friend class ObjectRoute;
    template <LockMode>
    friend class Guard;
    friend class IntrusiveList<KeySlot, InFlightListTag>;
    friend struct test::ObjectRouteTestPeer;

    struct Record {
        Generation generation;
        std::unique_ptr<ObjectMetadata> metadata;
        ObjectState state;
    };

    const std::string key_;
    mutable std::shared_mutex mutex_;
    std::optional<Record> record_;
    SlotOwnerState owner_;
};

// Called by the route as a write guard it handed out is released, with the slot
// locked: what the owner keeps about the key is settled there, so the code
// that changed the key need not do it.
class RouteObserver {
   public:
    virtual void OnWriteRelease(const WriteGuard& guard) = 0;

   protected:
    ~RouteObserver() = default;
};

// Holds one key, read or write, for the caller's scope. Only the route makes
// one. A write guard may hold a key with no object (see
// ObjectRoute::WriteOrCreate); a read guard always holds an object.
//
// A guard owns a strong reference to its slot, so the slot outlives its lock.
// It moves but does not assign, since assigning would drop one key's lock
// while taking another's.
template <LockMode kMode>
class Guard {
    static constexpr bool kWrite = kMode == LockMode::kWrite;
    using Lock = std::conditional_t<kWrite, std::unique_lock<std::shared_mutex>,
                                    std::shared_lock<std::shared_mutex>>;

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
    Metadata& metadata() const { return *record().metadata; }
    State& state() const { return record().state; }

    // Publishes `metadata` under this key, which holds no object, wiring its
    // group membership and the group's shared lease; the state starts empty.
    // Returns the new publication's generation.
    Generation Publish(std::unique_ptr<ObjectMetadata> metadata) const
        requires kWrite;

    // Ends the publication this guard holds: drops its group membership and
    // the record. The caller gives back whatever hangs off the object first,
    // under this same guard. A later Publish on the guard starts a new
    // publication of the key with nothing in between.
    void TearDown() const requires kWrite;

    // The owner's per-slot state, for the route's owner only.
    SlotOwnerState& owner_state() const requires kWrite {
        return slot_->owner_;
    }
    KeySlot& slot() const { return *slot_; }

   private:
    friend class ObjectRoute;

    Guard(ObjectRoute* route, std::shared_ptr<KeySlot> slot)
        NO_THREAD_SAFETY_ANALYSIS : route_(route),
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
    std::shared_ptr<KeySlot> slot_;
    Lock lock_;
    bool handed_out_ = false;
};

class ObjectRoute {
    // Keyed by a view of the slot's own key, which lives as long as the entry.
    using SlotMap =
        std::unordered_map<std::string_view, std::shared_ptr<KeySlot>,
                           TransparentStringHash, std::equal_to<>>;

   public:
    static constexpr size_t kStripeCount = 64;

    // `observer`, when set, outlives the route.
    explicit ObjectRoute(RouteObserver* observer = nullptr)
        : observer_(observer) {}
    ObjectRoute(const ObjectRoute&) = delete;
    ObjectRoute& operator=(const ObjectRoute&) = delete;

    // The object under `key`, held shared; nullopt when the key holds none.
    [[nodiscard]] std::optional<ReadGuard> Read(std::string_view key) const;
    // The same, only while `ref` is still the key's current publication.
    [[nodiscard]] std::optional<ReadGuard> Read(const ObjectRef& ref) const;

    // The object under `key`, held exclusively; nullopt when it holds none.
    [[nodiscard]] std::optional<WriteGuard> Write(std::string_view key);
    // The same, only while `ref` is still the key's current publication.
    [[nodiscard]] std::optional<WriteGuard> Write(const ObjectRef& ref);
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

    [[nodiscard]] std::shared_ptr<KeySlot> FindSlot(std::string_view key) const;
    [[nodiscard]] std::shared_ptr<KeySlot> FindOrCreateSlot(
        std::string_view key);
    // Drops `slot` from the map when it holds no object and `slot` is the
    // last reference outside the map. Called with no slot lock held.
    void Collect(std::shared_ptr<KeySlot> slot);

    Generation PublishLocked(KeySlot& slot,
                             std::unique_ptr<ObjectMetadata> metadata);
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
        using SlotLock =
            std::conditional_t<kWrite, std::unique_lock<std::shared_mutex>,
                               std::shared_lock<std::shared_mutex>>;

       public:
        // The object a position stands on, valid until the cursor advances.
        class Object {
           public:
            using Metadata = typename Guard<kMode>::Metadata;
            using State = typename Guard<kMode>::State;

            const std::string& key() const { return slot_.key(); }
            Generation generation() const {
                return slot_.record_->generation;
            }
            ObjectRef ref() const { return {key(), generation()}; }
            Metadata& metadata() const { return *slot_.record_->metadata; }
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
            slot_lock_ = SlotLock();
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
            slot_lock_ = SlotLock();
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
                const Stripe& stripe = route_.stripes_[stripe_];
                if (!stripe_lock_.owns_lock()) {
                    AssertNotInCursor();
                    stripe_lock_ =
                        std::shared_lock<std::shared_mutex>(stripe.lock);
                    ++cursor_stripes_;
                    slot_it_ = stripe.slots.begin();
                }
                for (; slot_it_ != stripe.slots.end(); ++slot_it_) {
                    KeySlot& slot = *slot_it_->second;
                    SlotLock lock(slot.mutex_, std::try_to_lock);
                    if (!lock.owns_lock()) {
                        deferred_.push_back(slot_it_->second);
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
                SlotLock lock(slot.mutex_);
                if (slot.record_.has_value()) {
                    Stand(slot, std::move(lock));
                    return;
                }
            }
            current_ = nullptr;
        }

        void Stand(KeySlot& slot, SlotLock lock) {
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
        typename SlotMap::const_iterator slot_it_;
        // Slots that were busy when the cursor passed their stripe; the
        // references keep them alive until they are visited.
        std::vector<std::shared_ptr<KeySlot>> deferred_;
        size_t deferred_pos_ = 0;
        SlotLock slot_lock_;
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
    }
    const bool empty = !slot_->record_.has_value();
    lock_.unlock();
    if (empty) {
        route_->Collect(std::move(slot_));
    }
}

template <LockMode kMode>
Generation Guard<kMode>::Publish(std::unique_ptr<ObjectMetadata> metadata) const
    requires kWrite
{
    assert(!has_object());
    assert(metadata != nullptr && metadata->user_key == key());
    return route_->PublishLocked(*slot_, std::move(metadata));
}

template <LockMode kMode>
void Guard<kMode>::TearDown() const requires kWrite
{
    assert(has_object());
    route_->TearDownLocked(*slot_);
}

// --- ObjectRoute ------------------------------------------------------------

inline std::shared_ptr<KeySlot> ObjectRoute::FindSlot(
    std::string_view key) const {
    AssertNotInCursor();
    Stripe& stripe = StripeOf(key);
    std::shared_lock<std::shared_mutex> lock(stripe.lock);
    const auto it = stripe.slots.find(key);
    return it == stripe.slots.end() ? nullptr : it->second;
}

inline std::shared_ptr<KeySlot> ObjectRoute::FindOrCreateSlot(
    std::string_view key) {
    if (auto slot = FindSlot(key)) {
        return slot;
    }
    Stripe& stripe = StripeOf(key);
    std::unique_lock<std::shared_mutex> lock(stripe.lock);
    if (const auto it = stripe.slots.find(key); it != stripe.slots.end()) {
        return it->second;
    }
    auto slot = std::make_shared<KeySlot>(std::string(key));
    stripe.slots.emplace(slot->key(), slot);
    return slot;
}

inline void ObjectRoute::Collect(std::shared_ptr<KeySlot> slot) {
    Stripe& stripe = StripeOf(slot->key());
    std::unique_lock<std::shared_mutex> lock(stripe.lock);
    // A new reference is only made under the stripe lock, so with the map's
    // and ours the only ones, nobody else can reach the slot. Locking it then
    // never waits, and it orders this read of the record after the last
    // writer's release.
    if (slot.use_count() != 2) {
        stripe.uncollected.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    std::unique_lock<std::shared_mutex> slot_lock(slot->mutex_,
                                                  std::try_to_lock);
    if (!slot_lock.owns_lock() || slot->record_.has_value()) {
        return;
    }
    stripe.slots.erase(slot->key());
}

inline std::optional<ReadGuard> ObjectRoute::Read(std::string_view key) const {
    auto slot = FindSlot(key);
    if (slot == nullptr) {
        return std::nullopt;
    }
    ReadGuard guard(const_cast<ObjectRoute*>(this), std::move(slot));
    if (!guard.has_object()) {
        return std::nullopt;
    }
    return std::optional<ReadGuard>(std::move(guard).HandOut());
}

inline std::optional<ReadGuard> ObjectRoute::Read(const ObjectRef& ref) const {
    auto slot = FindSlot(ref.key);
    if (slot == nullptr) {
        return std::nullopt;
    }
    ReadGuard guard(const_cast<ObjectRoute*>(this), std::move(slot));
    if (!guard.has_object() || guard.generation() != ref.generation) {
        return std::nullopt;
    }
    return std::optional<ReadGuard>(std::move(guard).HandOut());
}

inline std::optional<WriteGuard> ObjectRoute::Write(std::string_view key) {
    auto slot = FindSlot(key);
    if (slot == nullptr) {
        return std::nullopt;
    }
    WriteGuard guard(this, std::move(slot));
    if (!guard.has_object()) {
        return std::nullopt;
    }
    return std::optional<WriteGuard>(std::move(guard).HandOut());
}

inline std::optional<WriteGuard> ObjectRoute::Write(const ObjectRef& ref) {
    auto slot = FindSlot(ref.key);
    if (slot == nullptr) {
        return std::nullopt;
    }
    WriteGuard guard(this, std::move(slot));
    if (!guard.has_object() || guard.generation() != ref.generation) {
        return std::nullopt;
    }
    return std::optional<WriteGuard>(std::move(guard).HandOut());
}

inline WriteGuard ObjectRoute::WriteOrCreate(std::string_view key) {
    return WriteGuard(this, FindOrCreateSlot(key)).HandOut();
}

inline Generation ObjectRoute::PublishLocked(
    KeySlot& slot, std::unique_ptr<ObjectMetadata> metadata) {
    const Generation generation =
        next_generation_.fetch_add(1, std::memory_order_relaxed);
    if (!metadata->group_id.empty()) {
        // AddMember returns null only for an empty group_id.
        auto lease = groups_.AddMember(metadata->group_id, slot.key());
        assert(lease != nullptr);
        SpinLocker locker(&metadata->lock);
        metadata->lease_ = std::move(lease);
    }
    slot.record_.emplace(
        KeySlot::Record{generation, std::move(metadata), ObjectState{}});
    object_count_.fetch_add(1, std::memory_order_relaxed);
    return generation;
}

inline void ObjectRoute::TearDownLocked(KeySlot& slot) {
    const std::string& group_id = slot.record_->metadata->group_id;
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
            KeySlot& slot = *it->second;
            std::unique_lock<std::shared_mutex> slot_lock(slot.mutex_,
                                                          std::try_to_lock);
            if (!slot_lock.owns_lock() || slot.record_.has_value()) {
                // A holder collects the slot itself if it leaves it empty.
                ++it;
            } else if (it->second.use_count() != 1) {
                // Empty but still referenced, as by a cursor that set it
                // aside: try again on the next sweep.
                stripe.uncollected.fetch_add(1, std::memory_order_relaxed);
                ++it;
            } else {
                // Only the map references it, and nobody can make a new
                // reference while the stripe is held.
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
