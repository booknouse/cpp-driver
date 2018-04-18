/*
  Copyright (c) DataStax, Inc.

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

  http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.
*/

#include "session.hpp"

#include "batch_request.hpp"
#include "cluster.hpp"
#include "config.hpp"
#include "constants.hpp"
#include "execute_request.hpp"
#include "logger.hpp"
#include "prepare_request.hpp"
#include "request_processor_manager_initializer.hpp"
#include "scoped_lock.hpp"
#include "statement.hpp"
#include "timer.hpp"
#include "external.hpp"

extern "C" {

CassSession* cass_session_new() {
  return CassSession::to(cass::Memory::allocate<cass::Session>());
}

void cass_session_free(CassSession* session) { // This attempts to close the session because the joining will
  // hang indefinitely otherwise. This causes minimal delay
  // if the session is already closed.
  cass::SharedRefPtr<cass::Future> future(cass::Memory::allocate<cass::SessionFuture>());
  session->close_async(future);
  future->wait();

  cass::Memory::deallocate(session->from());
}

CassFuture* cass_session_connect(CassSession* session, const CassCluster* cluster) {
  return cass_session_connect_keyspace(session, cluster, "");
}

CassFuture* cass_session_connect_keyspace(CassSession* session,
                                          const CassCluster* cluster,
                                          const char* keyspace) {
  return cass_session_connect_keyspace_n(session,
                                         cluster,
                                         keyspace,
                                         SAFE_STRLEN(keyspace));
}

CassFuture* cass_session_connect_keyspace_n(CassSession* session,
                                            const CassCluster* cluster,
                                            const char* keyspace,
                                            size_t keyspace_length) {
  cass::SessionFuture::Ptr connect_future(cass::Memory::allocate<cass::SessionFuture>());
  session->connect_async(cluster->config(), cass::String(keyspace, keyspace_length), connect_future);
  connect_future->inc_ref();
  return CassFuture::to(connect_future.get());
}

CassFuture* cass_session_close(CassSession* session) {
  cass::SessionFuture::Ptr close_future(cass::Memory::allocate<cass::SessionFuture>());
  session->close_async(close_future);
  close_future->inc_ref();
  return CassFuture::to(close_future.get());
}

CassFuture* cass_session_prepare(CassSession* session, const char* query) {
  return cass_session_prepare_n(session, query, SAFE_STRLEN(query));
}

CassFuture* cass_session_prepare_n(CassSession* session,
                                   const char* query,
                                   size_t query_length) {
  cass::Future::Ptr future(session->prepare(query, query_length));
  future->inc_ref();
  return CassFuture::to(future.get());
}

CassFuture* cass_session_prepare_from_existing(CassSession* session,
                                               CassStatement* statement) {
  cass::Future::Ptr future(session->prepare(statement));
  future->inc_ref();
  return CassFuture::to(future.get());
}

CassFuture* cass_session_execute(CassSession* session,
                                 const CassStatement* statement) {
  cass::Future::Ptr future(session->execute(cass::Request::ConstPtr(statement->from())));
  future->inc_ref();
  return CassFuture::to(future.get());
}

CassFuture* cass_session_execute_batch(CassSession* session, const CassBatch* batch) {
  cass::Future::Ptr future(session->execute(cass::Request::ConstPtr(batch->from())));
  future->inc_ref();
  return CassFuture::to(future.get());
}

const CassSchemaMeta* cass_session_get_schema_meta(const CassSession* session) {
  return CassSchemaMeta::to(cass::Memory::allocate<cass::Metadata::SchemaSnapshot>(session->metadata().schema_snapshot(session->cassandra_version())));
}

void  cass_session_get_metrics(const CassSession* session,
                               CassMetrics* metrics) {
  const cass::Metrics* internal_metrics = session->metrics();

  cass::Metrics::Histogram::Snapshot requests_snapshot;
  internal_metrics->request_latencies.get_snapshot(&requests_snapshot);

  metrics->requests.min = requests_snapshot.min;
  metrics->requests.max = requests_snapshot.max;
  metrics->requests.mean = requests_snapshot.mean;
  metrics->requests.stddev = requests_snapshot.stddev;
  metrics->requests.median = requests_snapshot.median;
  metrics->requests.percentile_75th = requests_snapshot.percentile_75th;
  metrics->requests.percentile_95th = requests_snapshot.percentile_95th;
  metrics->requests.percentile_98th = requests_snapshot.percentile_98th;
  metrics->requests.percentile_99th = requests_snapshot.percentile_99th;
  metrics->requests.percentile_999th = requests_snapshot.percentile_999th;
  metrics->requests.one_minute_rate = internal_metrics->request_rates.one_minute_rate();
  metrics->requests.five_minute_rate = internal_metrics->request_rates.five_minute_rate();
  metrics->requests.fifteen_minute_rate = internal_metrics->request_rates.fifteen_minute_rate();
  metrics->requests.mean_rate = internal_metrics->request_rates.mean_rate();

  metrics->stats.total_connections = internal_metrics->total_connections.sum();
  metrics->stats.available_connections = metrics->stats.total_connections; // Deprecated
  metrics->stats.exceeded_write_bytes_water_mark = 0; // Deprecated
  metrics->stats.exceeded_pending_requests_water_mark = 0; // Deprecated

  metrics->errors.connection_timeouts = internal_metrics->connection_timeouts.sum();
  metrics->errors.pending_request_timeouts = internal_metrics->pending_request_timeouts.sum();
  metrics->errors.request_timeouts = internal_metrics->request_timeouts.sum();
}

void  cass_session_get_speculative_execution_metrics(const CassSession* session,
                                                     CassSpeculativeExecutionMetrics* metrics) {
  const cass::Metrics* internal_metrics = session->metrics();

  cass::Metrics::Histogram::Snapshot speculative_snapshot;
  internal_metrics->speculative_request_latencies.get_snapshot(&speculative_snapshot);

  metrics->min = speculative_snapshot.min;
  metrics->max = speculative_snapshot.max;
  metrics->mean = speculative_snapshot.mean;
  metrics->stddev = speculative_snapshot.stddev;
  metrics->median = speculative_snapshot.median;
  metrics->percentile_75th = speculative_snapshot.percentile_75th;
  metrics->percentile_95th = speculative_snapshot.percentile_95th;
  metrics->percentile_98th = speculative_snapshot.percentile_98th;
  metrics->percentile_99th = speculative_snapshot.percentile_99th;
  metrics->percentile_999th = speculative_snapshot.percentile_999th;
  metrics->count =
      internal_metrics->request_rates.speculative_request_count();
  metrics->percentage =
      internal_metrics->request_rates.speculative_request_percent();
}

} // extern "C"

namespace cass {

Session::Session()
  : state_(SESSION_STATE_CLOSED)
  , connect_error_code_(CASS_OK)
  , current_host_mark_(true) {
  uv_mutex_init(&state_mutex_);
  uv_mutex_init(&hosts_mutex_);
}

Session::~Session() {
  join();
  uv_mutex_destroy(&state_mutex_);
  uv_mutex_destroy(&hosts_mutex_);
}

void Session::clear(const Config& config) {
  config_ = config.new_instance();
  random_.reset();
  metrics_.reset(Memory::allocate<Metrics>(config_.thread_count_io() + 1));
  connect_future_.reset();
  close_future_.reset();
  { // Lock hosts
    ScopedMutex l(&hosts_mutex_);
    hosts_.clear();
  }
  request_queue_.reset();
  request_processor_manager_.reset();
  metadata_.clear();
  control_connection_.clear();
  connect_error_code_ = CASS_OK;
  connect_error_message_.clear();
  current_host_mark_ = true;
}

int Session::init() {
  int rc = EventLoop::init("Session");

  event_loop_group_.reset(Memory::allocate<RoundRobinEventLoopGroup>(config_.thread_count_io()));
  rc = event_loop_group_->init("Request Processor");

  return rc;
}

Host::Ptr Session::get_host(const Address& address) {
  // Lock hosts. This can be called on a non-session thread.
  ScopedMutex l(&hosts_mutex_);
  HostMap::iterator it = hosts_.find(address);
  if (it == hosts_.end()) {
    return Host::Ptr();
  }
  return it->second;
}

Host::Ptr Session::add_host(const Address& address) {
  LOG_DEBUG("Adding new host: %s", address.to_string().c_str());
  Host::Ptr host(Memory::allocate<Host>(address, !current_host_mark_));
  { // Lock hosts
    ScopedMutex l(&hosts_mutex_);
    hosts_[address] = host;
  }

  //TODO(CPP-404): Remove NULL check once integrated with CPP-453?
  if (request_processor_manager_) request_processor_manager_->notify_host_add_async(host);
  return host;
}

void Session::purge_hosts(bool is_initial_connection) {
  // Hosts lock not held for reading (only called on session thread)
  HostMap::iterator it = hosts_.begin();
  while (it != hosts_.end()) {
    if (it->second->mark() != current_host_mark_) {
      HostMap::iterator to_remove_it = it++;

      String address_str = to_remove_it->first.to_string();
      if (is_initial_connection) {
        LOG_WARN("Unable to reach contact point %s", address_str.c_str());
        { // Lock hosts
          ScopedMutex l(&hosts_mutex_);
          hosts_.erase(to_remove_it);
        }
      } else {
        LOG_WARN("Host %s removed", address_str.c_str());
        on_remove(to_remove_it->second);
      }
    } else {
      ++it;
    }
  }
  current_host_mark_ = !current_host_mark_;
}

bool Session::prepare_host(const Host::Ptr& host,
                           PrepareHostHandler::Callback callback) {
  if (config_.prepare_on_up_or_add_host()) {
    PrepareHostHandler::Ptr prepare_host_handler(
          Memory::allocate<PrepareHostHandler>(host,
                                               this,
                                               control_connection_.protocol_version()));
    prepare_host_handler->prepare(callback);
    return true;
  }
  return false;
}

void Session::on_prepare_host_add(const PrepareHostHandler* handler) {
  handler->session()->internal_on_add(handler->host());
}

void Session::on_prepare_host_up(const PrepareHostHandler* handler) {
  handler->session()->internal_on_up(handler->host());
}

void Session::connect_async(const Config& config, const String& keyspace, const Future::Ptr& future) {
  ScopedMutex l(&state_mutex_);

  if (state_.load(MEMORY_ORDER_RELAXED) != SESSION_STATE_CLOSED) {
    future->set_error(CASS_ERROR_LIB_UNABLE_TO_CONNECT,
                      "Already connecting, connected or closed");
    return;
  }

  clear(config);

  if (init() != 0) {
    future->set_error(CASS_ERROR_LIB_UNABLE_TO_INIT,
                      "Error initializing session");
    return;
  }

  state_.store(SESSION_STATE_CONNECTING, MEMORY_ORDER_RELAXED);
  connect_keyspace_ = keyspace;
  connect_future_ = future;

  add(Memory::allocate<NotifyConnect>());

  LOG_DEBUG("Issued connect event");

  // If this is a reconnect then the old thread needs to be
  // joined before creating a new thread.
  join();

  run();
}

void Session::close_async(const Future::Ptr& future) {
  ScopedMutex l(&state_mutex_);

  State state = state_.load(MEMORY_ORDER_RELAXED);
  if (state == SESSION_STATE_CLOSING || state == SESSION_STATE_CLOSED) {
    future->set_error(CASS_ERROR_LIB_UNABLE_TO_CLOSE,
                      "Already closing or closed");
    return;
  }

  state_.store(SESSION_STATE_CLOSING, MEMORY_ORDER_RELAXED);
  close_future_ = future;

  internal_close();
}

void Session::internal_connect() {
  if (hosts_.empty()) { // No hosts lock necessary (only called on session thread)
    notify_connect_error(CASS_ERROR_LIB_NO_HOSTS_AVAILABLE,
                         "No hosts provided or no hosts resolved");
    return;
  }
  control_connection_.connect(this);
}

void Session::internal_close() {
  if (request_processor_manager_) request_processor_manager_->close();
  control_connection_.close();
  close_handles();

  LOG_DEBUG("Issued close");
}

void Session::notify_connected() {
  LOG_DEBUG("Session is connected");

  ScopedMutex l(&state_mutex_);

  if (state_.load(MEMORY_ORDER_RELAXED) == SESSION_STATE_CONNECTING) {
    state_.store(SESSION_STATE_CONNECTED, MEMORY_ORDER_RELAXED);
  }
  if (connect_future_) {
    connect_future_->set();
    connect_future_.reset();
  }
}

void Session::notify_connect_error(CassError code, const String& message) {
  ScopedMutex l(&state_mutex_);

  State state = state_.load(MEMORY_ORDER_RELAXED);
  if (state == SESSION_STATE_CLOSING || state == SESSION_STATE_CLOSED) {
    return;
  }

  state_.store(SESSION_STATE_CLOSING, MEMORY_ORDER_RELAXED);
  connect_error_code_ = code;
  connect_error_message_ = message;

  internal_close();
}

void Session::notify_closed() {
  LOG_DEBUG("Session is disconnected");

  ScopedMutex l(&state_mutex_);

  state_.store(SESSION_STATE_CLOSED, MEMORY_ORDER_RELAXED);
  if (connect_future_) {
    connect_future_->set_error(connect_error_code_, connect_error_message_);
    connect_future_.reset();
  }
  if (close_future_) {
    close_future_->set();
    close_future_.reset();
  }
}

void Session::close_handles() {
  EventLoop::close_handles();
  if (request_processor_manager_) request_processor_manager_->close_handles();
  event_loop_group_->close_handles();
  config().default_profile().load_balancing_policy()->close_handles();
}

void Session::on_run() {
  LOG_DEBUG("Creating %u IO worker threads",
            static_cast<unsigned int>(config_.thread_count_io()));
  event_loop_group_->run();
}

void Session::on_after_run() {
  event_loop_group_->join();
  notify_closed();
}

void Session::NotifyConnect::run(EventLoop* event_loop) {
  Session* session = static_cast<Session*>(event_loop);
  session->handle_notify_connect();
}

void Session::handle_notify_connect() {
  int port = config_.port();

  // This needs to be done on the session thread because it could pause
  // generating a new random seed.
  if (config_.use_randomized_contact_points()) {
    random_.reset(Memory::allocate<Random>());
  }

  MultiResolver<Session*>::Ptr resolver(
      Memory::allocate<MultiResolver<Session*> >(this, on_resolve,
                                                 on_resolve_done));

  const ContactPointList& contact_points = config_.contact_points();
  for (ContactPointList::const_iterator it = contact_points.begin(),
       end = contact_points.end();
       it != end; ++it) {
    const String& seed = *it;
    Address address;
    if (Address::from_string(seed, port, &address)) {
      add_host(address);
    } else {
      resolver->resolve(loop(), seed, port, config_.resolve_timeout_ms());
    }
  }
}

void Session::on_resolve(MultiResolver<Session*>::Resolver* resolver) {
  Session* session = resolver->data()->data();
  if (resolver->is_success()) {
    AddressVec addresses = resolver->addresses();
    for (AddressVec::iterator it = addresses.begin(); it != addresses.end(); ++it) {
      Host::Ptr host = session->add_host(*it);
      host->set_hostname(resolver->hostname());
    }
  } else if (resolver->is_timed_out()) {
    LOG_ERROR("Timed out attempting to resolve address for %s:%d\n",
              resolver->hostname().c_str(), resolver->port());
  } else {
    LOG_ERROR("Unable to resolve address for %s:%d\n",
              resolver->hostname().c_str(), resolver->port());
  }
}

void Session::on_resolve_done(MultiResolver<Session*>* resolver) {
  resolver->data()->internal_connect();
}

void Session::execute(const RequestHandler::Ptr& request_handler) {
  if (state_.load(MEMORY_ORDER_ACQUIRE) != SESSION_STATE_CONNECTED) {
    request_handler->set_error(CASS_ERROR_LIB_NO_HOSTS_AVAILABLE,
                               "Session is not connected");
    return;
  }

  request_handler->inc_ref(); // Queue reference
  if (request_queue_->enqueue(request_handler.get())) {
    request_processor_manager_->notify_request_async();
  } else {
    request_handler->dec_ref();
    request_handler->set_error(CASS_ERROR_LIB_REQUEST_QUEUE_FULL,
                               "The request queue has reached capacity");
  }
}

bool Session::token_map_init(const String& partitioner) {
  if (!token_map_) {
    token_map_.reset(TokenMap::from_partitioner(partitioner));
    return true;
  }
  return false;
}

void Session::token_map_host_add(const Host::Ptr& host, const Value* tokens) {
  if (token_map_) {
    token_map_->add_host(host, tokens);
    notify_token_map_update();
  }
}

void Session::token_map_host_update(const Host::Ptr& host, const Value* tokens) {
  if (token_map_) {
    token_map_->update_host_and_build(host, tokens);
    notify_token_map_update();
  }
}

void Session::token_map_host_remove(const Host::Ptr& host) {
  if (token_map_) {
    token_map_->remove_host_and_build(host);
    notify_token_map_update();
  }
}

void Session::token_map_hosts_cleared() {
  if (token_map_) {
    token_map_->clear_tokens_and_hosts();
    notify_token_map_update();
  }
}

void Session::token_map_keyspaces_add(const VersionNumber& cassandra_version,
                                      const ResultResponse::Ptr& keyspaces) {
  if (token_map_) {
    if (keyspaces) {
      token_map_->clear_replicas_and_strategies(); // Only clear replicas once we have the new keyspaces
      token_map_->add_keyspaces(cassandra_version, keyspaces.get());
    }
    token_map_->build();
    notify_token_map_update();
  }
}

void Session::token_map_keyspaces_update(const VersionNumber& cassandra_version,
                                         const ResultResponse::Ptr& keyspaces) {
  if (token_map_ && keyspaces) {
    token_map_->update_keyspaces_and_build(cassandra_version, keyspaces.get());
    notify_token_map_update();
  }
}

void Session::load_balancing_policy_host_add_remove(const Host::Ptr& host,
                                                    bool is_add) {
  LoadBalancingPolicy::Ptr default_policy = config().default_profile().load_balancing_policy();
  if (is_add) {
    default_policy->on_add(host);
  } else {
    default_policy->on_remove(host);
  }
}

void Session::notify_token_map_update() {
  //TODO(CPP-404): Remove NULL check once integrated with CPP-453?
  if (request_processor_manager_) request_processor_manager_->notify_token_map_update_async(token_map_.get());
}

void Session::on_control_connection_ready() {
  // No hosts lock necessary (only called on session thread and read-only)
  const Host::Ptr connected_host = control_connection_.connected_host();
  int protocol_version = control_connection_.protocol_version();
  Random* random = random_.get();
  config().default_profile().load_balancing_policy()->init(connected_host, hosts_, random);
  config().default_profile().load_balancing_policy()->register_handles(loop());

  request_queue_.reset(Memory::allocate<MPMCQueue<RequestHandler*> >(config_.queue_size_io()));
  RequestProcessorManagerInitializer::Ptr initializer(Memory::allocate<RequestProcessorManagerInitializer>(this,
                                                                                                           on_request_processor_manager_initialize));
  initializer->with_settings(RequestProcessorManagerSettings(config_))
    ->with_connect_keyspace(connect_keyspace_)
    ->with_default_profile(config_.default_profile())
    ->with_event_loop_group(event_loop_group_.get())
    ->with_hosts(connected_host, hosts_)
    ->with_listener(this)
    ->with_max_schema_wait_time_ms(config_.max_schema_wait_time_ms())
    ->with_metrics(metrics_.get())
    ->with_prepare_statements_on_all(config_.prepare_on_all_hosts())
    ->with_profiles(config_.profiles())
    ->with_protocol_version(protocol_version)
    ->with_randomized_contact_points(config_.use_randomized_contact_points())
    ->with_request_queue(request_queue_.get())
    ->with_timestamp_generator(config_.timestamp_gen())
    ->with_token_map(token_map_.get())
    ->initialize();

  // TODO: We really can't do this anymore
  if (config().core_connections_per_host() == 0) {
    // Special case for internal testing. Not allowed by API
    LOG_DEBUG("Session connected with no core IO connections");
  }
}

void Session::on_control_connection_error(CassError code, const String& message) {
  notify_connect_error(code, message);
}

Future::Ptr Session::prepare(const char* statement, size_t length) {
  PrepareRequest::Ptr prepare(Memory::allocate<PrepareRequest>(String(statement, length)));

  ResponseFuture::Ptr future(Memory::allocate<ResponseFuture>(metadata_.schema_snapshot(cassandra_version())));
  future->prepare_request = PrepareRequest::ConstPtr(prepare);

  execute(RequestHandler::Ptr(Memory::allocate<RequestHandler>(prepare,
                                                               future,
                                                               prepared_metadata(),
                                                               metrics_.get())));

  return future;
}

Future::Ptr Session::prepare(const Statement* statement) {
  String query;

  if (statement->opcode() == CQL_OPCODE_QUERY) { // Simple statement
    query = statement->query();
  } else { // Bound statement
    query = static_cast<const ExecuteRequest*>(statement)->prepared()->query();
  }

  PrepareRequest::Ptr prepare(Memory::allocate<PrepareRequest>(query));

  // Inherit the settings of the existing statement. These will in turn be
  // inherited by bound statements.
  prepare->set_settings(statement->settings());

  ResponseFuture::Ptr future(Memory::allocate<ResponseFuture>(metadata_.schema_snapshot(cassandra_version())));
  future->prepare_request = PrepareRequest::ConstPtr(prepare);

  execute(RequestHandler::Ptr(Memory::allocate<RequestHandler>(prepare,
                                                               future,
                                                               prepared_metadata(),
                                                               metrics_.get())));

  return future;
}

void Session::on_add(Host::Ptr host) {
  host->set_up(); // Set the host as up immediately (to avoid duplicate actions)

  if (!prepare_host(host, on_prepare_host_add)) {
    internal_on_add(host);
  }
}

void Session::internal_on_add(Host::Ptr host) {
  // Verify that the host is still available
  if (host->is_down()) return;

  config().default_profile().load_balancing_policy()->on_add(host);

  request_processor_manager_->notify_host_add_async(host);
}

void Session::on_remove(Host::Ptr host) {
  host->set_down();

  config().default_profile().load_balancing_policy()->on_remove(host);
  { // Lock hosts
    ScopedMutex l(&hosts_mutex_);
    hosts_.erase(host->address());
  }

  request_processor_manager_->notify_host_remove_async(host);
}

void Session::on_up(Host::Ptr host) {
  host->set_up(); // Set the host as up immediately (to avoid duplicate actions)

  if (!prepare_host(host, on_prepare_host_up)) {
    internal_on_up(host);
  }
}

void Session::internal_on_up(Host::Ptr host) {
  // Verify that the host is still available
  if (host->is_down()) return;

  config().default_profile().load_balancing_policy()->on_up(host);
}

void Session::on_down(Host::Ptr host) {
  host->set_down();

  config().default_profile().load_balancing_policy()->on_down(host);
}

void Session::on_request_processor_manager_initialize(RequestProcessorManagerInitializer* initializer) {
  Session* session = static_cast<Session*>(initializer->data());
  session->handle_request_processor_manager_initialize(initializer->release_manager(),
                                                       initializer->failures());
}

void Session::handle_request_processor_manager_initialize(const RequestProcessorManager::Ptr& request_processor_manager,
                                                          const RequestProcessor::Vec& failures) {
  request_processor_manager_ = request_processor_manager;

  // Check for failed connection(s) in the request processor(s)
  if (!failures.empty()) {
    // All failures should be the same, just pass the first error
    notify_connect_error(failures[0]->error_code(),
                         failures[0]->error_message());
  } else {
    notify_connected();
  }
}

void Session::on_keyspace_update(const String& keyspace) {
  request_processor_manager_->keyspace_update(keyspace);
}

void Session::on_prepared_metadata_update(const String& id,
                                          const PreparedMetadata::Entry::Ptr& entry) {
  // No need for a read-write lock; PreparedMetadata handles this
  prepared_metadata_.set(id, entry);
}

Future::Ptr Session::execute(const Request::ConstPtr& request,
                             const Address* preferred_address) {
  ResponseFuture::Ptr future(Memory::allocate<ResponseFuture>());

  execute(RequestHandler::Ptr(Memory::allocate<RequestHandler>(request,
                                                               future,
                                                               prepared_metadata(),
                                                               metrics_.get(),
                                                               preferred_address)));

  return future;
}

QueryPlan* Session::new_query_plan() {
  return config_.default_profile().load_balancing_policy()->new_query_plan("",
                                                                           NULL,
                                                                           token_map_.get());
}

} // namespace cass
