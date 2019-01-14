#include <storages/postgres/detail/cluster_impl.hpp>

#include <engine/async.hpp>

#include <storages/postgres/detail/topology_discovery.hpp>
#include <storages/postgres/detail/topology_proxy.hpp>
#include <storages/postgres/dsn.hpp>
#include <storages/postgres/exceptions.hpp>

namespace storages {
namespace postgres {
namespace detail {

namespace {

constexpr const char* kPeriodicTaskName = "pg_topology";

std::string HostAndPortFromDsn(const std::string& dsn) {
  auto options = OptionsFromDsn(dsn);
  return options.host + ':' + options.port;
}

struct TryLockGuard {
  TryLockGuard(std::atomic_flag& lock) : lock_(lock) {
    lock_acquired_ = !lock_.test_and_set(std::memory_order_acq_rel);
  }

  ~TryLockGuard() {
    if (lock_acquired_) {
      lock_.clear(std::memory_order_release);
    }
  }

  bool LockAcquired() const { return lock_acquired_; }

 private:
  std::atomic_flag& lock_;
  bool lock_acquired_;
};

}  // namespace

ClusterImpl::ClusterImpl(const ClusterDescription& cluster_desc,
                         engine::TaskProcessor& bg_task_processor,
                         size_t initial_size, size_t max_size)
    : ClusterImpl(bg_task_processor, initial_size, max_size) {
  topology_ = std::make_unique<ClusterTopologyProxy>(cluster_desc);
  const auto& dsn_list =
      static_cast<ClusterTopologyProxy*>(topology_.get())->GetDsnList();
  InitPools(dsn_list);
}

ClusterImpl::ClusterImpl(const DSNList& dsn_list,
                         engine::TaskProcessor& bg_task_processor,
                         size_t initial_size, size_t max_size)
    : ClusterImpl(bg_task_processor, initial_size, max_size) {
  topology_ =
      std::make_unique<ClusterTopologyDiscovery>(bg_task_processor_, dsn_list);
  InitPools(dsn_list);
  StartPeriodicUpdates();
}

ClusterImpl::ClusterImpl(engine::TaskProcessor& bg_task_processor,
                         size_t initial_size, size_t max_size)
    : bg_task_processor_(bg_task_processor),
      host_ind_(0),
      pool_initial_size_(initial_size),
      pool_max_size_(max_size),
      update_lock_ ATOMIC_FLAG_INIT {}

ClusterImpl::~ClusterImpl() { StopPeriodicUpdates(); }

void ClusterImpl::StartPeriodicUpdates() {
  using ::utils::PeriodicTask;
  using Flags = ::utils::PeriodicTask::Flags;

  // TODO remove ugly constant
  PeriodicTask::Settings settings(ClusterTopologyDiscovery::kUpdateInterval,
                                  {Flags::kNow, Flags::kStrong});
  periodic_task_.Start(kPeriodicTaskName, settings,
                       [this] { CheckTopology(); });
}

void ClusterImpl::StopPeriodicUpdates() { periodic_task_.Stop(); }

void ClusterImpl::InitPools(const DSNList& dsn_list) {
  std::vector<engine::TaskWithResult<std::pair<std::string, ConnectionPoolPtr>>>
      tasks;
  tasks.reserve(dsn_list.size());
  HostPoolByDsn host_pools;
  host_pools.reserve(dsn_list.size());

  // TODO this code may be simplified when we don't block on pool initialization
  for (auto dsn : dsn_list) {
    auto task = engine::Async([ this, dsn = std::move(dsn) ] {
      return std::make_pair(dsn, std::make_shared<ConnectionPool>(
                                     dsn, bg_task_processor_,
                                     pool_initial_size_, pool_max_size_));
    });
    tasks.push_back(std::move(task));
  }

  for (auto&& task : tasks) {
    host_pools.insert(task.Get());
  }

  host_pools_.Set(std::make_shared<HostPoolByDsn>(std::move(host_pools)));
}

void ClusterImpl::CheckTopology() {
  TryLockGuard lock(update_lock_);
  if (!lock.LockAcquired()) {
    LOG_DEBUG() << "Already checking cluster topology";
    return;
  }

  // Copy pools first
  auto host_pools = *host_pools_.Get();
  const auto hosts_availability = topology_->CheckTopology();
  for (const auto & [ dsn, avail ] : hosts_availability) {
    switch (avail) {
      case ClusterTopology::HostAvailability::kOffline:
        host_pools.erase(dsn);
        LOG_DEBUG() << "Removed pool for host=" << HostAndPortFromDsn(dsn)
                    << " from the map";
        break;
      case ClusterTopology::HostAvailability::kPreOnline:
        host_pools[dsn] = std::make_shared<ConnectionPool>(
            dsn, bg_task_processor_, pool_initial_size_, pool_max_size_);
        LOG_DEBUG() << "Added pool for host=" << HostAndPortFromDsn(dsn)
                    << " to the map";
        break;
      case ClusterTopology::HostAvailability::kOnline:
        // Do nothing, we've already created the pool in pre-online phase
        break;
    }
  }
  // Set pools atomically
  host_pools_.Set(std::make_shared<HostPoolByDsn>(std::move(host_pools)));
}

ClusterImpl::ConnectionPoolPtr ClusterImpl::GetPool(
    const std::string& dsn) const {
  // Operate on the same extracted pool map to guarantee atomicity
  // Obtain and keep shared pointer to prolong lifetime of the pool map object
  const auto host_pools_ptr = host_pools_.Get();
  auto it_find = host_pools_ptr->find(dsn);
  return it_find == host_pools_ptr->end() ? nullptr : it_find->second;
}

ClusterStatistics ClusterImpl::GetStatistics() const {
  ClusterStatistics cluster_stats;
  auto hosts_by_type = topology_->GetHostsByType();

  // TODO remove code duplication
  const auto& master_dsns = hosts_by_type[ClusterHostType::kMaster];
  if (!master_dsns.empty()) {
    auto dsn = master_dsns[0];
    cluster_stats.master.dsn = dsn;
    if (auto pool = GetPool(dsn)) {
      cluster_stats.master.stats = pool->GetStatistics();
    }
  }

  const auto& sync_slave_dsns = hosts_by_type[ClusterHostType::kSyncSlave];
  if (!sync_slave_dsns.empty()) {
    auto dsn = sync_slave_dsns[0];
    cluster_stats.sync_slave.dsn = dsn;
    if (auto pool = GetPool(dsn)) {
      cluster_stats.sync_slave.stats = pool->GetStatistics();
    }
  }

  const auto& slaves_dsns = hosts_by_type[ClusterHostType::kSlave];
  if (!slaves_dsns.empty()) {
    cluster_stats.slaves.reserve(slaves_dsns.size());
    for (auto&& dsn : slaves_dsns) {
      InstanceStatsDescriptor slave_desc;
      slave_desc.dsn = dsn;
      if (auto pool = GetPool(dsn)) {
        slave_desc.stats = pool->GetStatistics();
      }
      cluster_stats.slaves.push_back(std::move(slave_desc));
    }
  }
  return cluster_stats;
}

Transaction ClusterImpl::Begin(ClusterHostType ht,
                               const TransactionOptions& options) {
  LOG_TRACE() << "Requested transaction on the host of " << ht << " type";
  auto host_type = ht;
  if (options.IsReadOnly()) {
    if (host_type == ClusterHostType::kAny) {
      host_type = ClusterHostType::kSyncSlave;
    }
  } else {
    if (host_type == ClusterHostType::kAny) {
      host_type = ClusterHostType::kMaster;
    } else if (host_type != ClusterHostType::kMaster) {
      throw ClusterUnavailable("Cannot start RW-transaction on a slave");
    }
  }

  auto hosts_by_type = topology_->GetHostsByType();
  const auto& host_dsns = hosts_by_type[host_type];
  if (host_dsns.empty()) {
    throw ClusterUnavailable("Pool for host type (passed: " + ToString(ht) +
                             ", picked: " + ToString(host_type) +
                             ") is not available");
  }

  const auto& dsn =
      host_dsns[host_ind_.fetch_add(1, std::memory_order_relaxed) %
                host_dsns.size()];
  LOG_TRACE() << "Starting transaction on the host of " << host_type << " type";

  auto pool = GetPool(dsn);
  if (!pool) {
    throw ClusterUnavailable("Host not found for given DSN: " + dsn);
  }

  try {
    return pool->Begin(options);
  } catch (const ConnectionError&) {
    topology_->OperationFailed(dsn);
    throw;
  }
}

}  // namespace detail
}  // namespace postgres
}  // namespace storages
