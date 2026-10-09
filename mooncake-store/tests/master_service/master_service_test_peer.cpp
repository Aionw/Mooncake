#include "master_service/master_service_test_peer.h"

#include <algorithm>
#include <cassert>

#include "ha/snapshot/catalog/snapshot_catalog_store.h"
#ifdef USE_NOF
#include "spdk/spdk_wrapper.h"
#endif

namespace mooncake::test {

ErrorCode MasterServiceTestPeer::SetBatchOpLogBackendForTesting(
    std::shared_ptr<HaKvBackend> backend) {
    // Explicit test injection keeps the zero-view fixture API. A configured
    // view still exercises the production fenced path.
    return service_.InitializeBatchOpLogWriter(std::move(backend),
                                               service_.view_version_ > 0);
}

void MasterServiceTestPeer::SetBatchOpLogWriterFactoryForTesting(
    MasterService::BatchOpLogWriterFactory factory) {
    assert(factory);
    assert(!service_.ordered_oplog_writer_);
    service_.batch_oplog_writer_factory_ = std::move(factory);
}

void MasterServiceTestPeer::RunBatchEvictForTesting(
    double evict_ratio_target, double evict_ratio_lowerbound) {
    service_.BatchEvict(evict_ratio_target, evict_ratio_lowerbound);
}

void MasterServiceTestPeer::RunNoFBatchEvictForTesting(
    double evict_ratio_target, double evict_ratio_lowerbound) {
    service_.NoFBatchEvict(evict_ratio_target, evict_ratio_lowerbound);
}

void MasterServiceTestPeer::RunDfsEvictionForTesting() {
    service_.RunDfsEviction();
}

void MasterServiceTestPeer::RunTenantEvictForTesting() {
    // The policy throttles its pass; a test asks for one now.
    service_.tenant_quota_policy_->next_eviction_check_ = {};
    service_.tenant_quota_policy_->OnMaintenance(service_.policy_store_access_);
}

void MasterServiceTestPeer::SetKvTenantEpochTrackingForTesting(bool enabled) {
    service_.kv_track_tenant_epochs_ = enabled;
}

void MasterServiceTestPeer::SetRemoveAllTenantHookForTesting(
    std::function<void(size_t)> hook) {
    service_.kv_remove_all_tenant_hook_ = std::move(hook);
}

uint64_t MasterServiceTestPeer::GetKvClearedPublishedForTesting() const {
    return service_.kv_cleared_published_.load(std::memory_order_relaxed);
}

uint64_t MasterServiceTestPeer::GetKvClearedSuppressedForTesting() const {
    return service_.kv_cleared_suppressed_by_epoch_.load(
        std::memory_order_relaxed);
}

void MasterServiceTestPeer::SetNoFProbeFnForTesting(
    MasterService::NoFProbeFn fn) {
#ifdef USE_NOF
    std::lock_guard<std::mutex> lock(service_.nof_probe_fn_mutex_);
    if (fn) {
        service_.nof_probe_fn_ = std::move(fn);
        return;
    }
    service_.nof_probe_fn_ = [](const std::string& te_endpoint,
                                uint32_t timeout_ms,
                                std::string* error_reason) {
        return SpdkWrapper::GetInstance().ProbeNofSegment(
            te_endpoint, timeout_ms, error_reason);
    };
#else
    (void)fn;
#endif
}

size_t MasterServiceTestPeer::GetMountedNoFSegmentCountForTesting() {
    std::vector<MountedNoFSegmentSnapshot> mounted_segments;
    service_.nof_segment_manager_.GetMountedSegmentsSnapshot(mounted_segments);
    return mounted_segments.size();
}

bool MasterServiceTestPeer::IsNoFSegmentMountedForTesting(
    const UUID& segment_id) {
    std::vector<MountedNoFSegmentSnapshot> mounted_segments;
    service_.nof_segment_manager_.GetMountedSegmentsSnapshot(mounted_segments);
    return std::any_of(
        mounted_segments.begin(), mounted_segments.end(),
        [&segment_id](const MountedNoFSegmentSnapshot& snapshot) {
            return snapshot.segment_id == segment_id &&
                   snapshot.status == SegmentStatus::OK;
        });
}

std::optional<uint32_t>
MasterServiceTestPeer::GetNoFHeartbeatFailureCountForTesting(
    const UUID& segment_id) {
    std::lock_guard<std::mutex> lock(service_.nof_heartbeat_mutex_);
    auto it = service_.nof_heartbeat_states_.find(segment_id);
    if (it == service_.nof_heartbeat_states_.end()) {
        return std::nullopt;
    }
    return it->second.consecutive_failures;
}

size_t MasterServiceTestPeer::RunPromotionCandidateRetryForTesting() {
    return service_.RunPromotionCandidateRetry();
}

void MasterServiceTestPeer::SeedPromotionTaskForTesting(
    const TenantId& tenant_id, const std::string& key, const UUID& holder_id,
    ReplicaID alloc_id, uint64_t object_size) {
    metadata::Namespace& ns = service_.namespaces_.GetOrCreate(tenant_id);
    // The route is what keeps the object reachable by the completion path, so
    // an absent key is seeded with a publication of its own.
    const auto hold = ns.objects.WriteOrCreate(key);
    if (!hold.has_object()) {
        hold.Publish(std::make_unique<ObjectMetadata>(
            holder_id, std::chrono::system_clock::now(), object_size,
            std::vector<Replica>{}, std::nullopt, false,
            ObjectDataType::UNKNOWN, std::string{}, tenant_id, key));
    }
    hold.state().promotion_task =
        PromotionTask{.source_id = 0,
                      .alloc_id = alloc_id,
                      .object_size = object_size,
                      .start_time = std::chrono::system_clock::now(),
                      .holder_id = holder_id};
}

size_t MasterServiceTestPeer::CountCandidatesForTesting(
    const TenantId& tenant_id) {
    std::shared_lock<std::shared_mutex> lock(service_.snapshot_mutex_);
    // The count is the tenant's candidate index; the candidate state itself
    // lives on each entry.
    return service_.promotion_candidates_.Keys(tenant_id).size();
}

void MasterServiceTestPeer::ResetCandidateBackoffsForTesting() {
    const auto epoch = std::chrono::steady_clock::time_point{};
    // The index only names keys, so each key is resolved again under its own
    // lock; a key that holds no object any more is skipped.
    service_.namespaces_.Visit(
        [&](const TenantId& tenant_id, metadata::Namespace& ns) {
            for (const auto& key :
                 service_.promotion_candidates_.Keys(tenant_id)) {
                auto hold = ns.objects.Write(key);
                if (hold && hold->state().promotion_candidate.has_value()) {
                    hold->state().promotion_candidate->retry_after = epoch;
                }
            }
        });
}

size_t MasterServiceTestPeer::SoftPinHeapSize() const {
    const auto& index = service_.soft_pin_deadline_index_;
    std::lock_guard lock(index.mutex_);
    return index.heap_.size();
}

size_t MasterServiceTestPeer::SoftPinRegistrationCount() const {
    const auto& index = service_.soft_pin_deadline_index_;
    std::lock_guard lock(index.mutex_);
    return index.registrations_.size();
}

std::unique_ptr<ha::SnapshotCatalogStore>
MasterServiceTestPeer::CreateSnapshotCatalogStore(
    const MasterServiceConfig& config) {
    return service_.CreateSnapshotCatalogStore(config);
}

}  // namespace mooncake::test
