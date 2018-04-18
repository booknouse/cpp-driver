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

#include "request_processor.hpp"

#include "connection_pool_manager_initializer.hpp"
#include "prepare_all_handler.hpp"
#include "request_queue.hpp"
#include "session.hpp"

namespace cass {

RequestProcessor::RequestProcessor(EventLoop* event_loop,
                                   const String& connect_keyspace,
                                   RequestProcessorListener* listener,
                                   unsigned max_schema_wait_time_ms,
                                   bool prepare_on_all_hosts,
                                   MPMCQueue<RequestHandler*>* request_queue,
                                   TimestampGenerator* timestamp_generator,
                                   void* data,
                                   Callback callback)
  : data_(data)
  , callback_(callback)
  , error_code_(CASS_OK)
  , connect_keyspace_(connect_keyspace)
  , listener_(listener)
  , max_schema_wait_time_ms_(max_schema_wait_time_ms)
  , prepare_on_all_hosts_(prepare_on_all_hosts)
  , request_queue_(request_queue)
  , timestamp_generator_(timestamp_generator)
  , event_loop_(event_loop)
  , is_flushing_(false)
  , is_closing_(false) { }

void RequestProcessor::close() {
  internal_close();
}

void RequestProcessor::close_handles() {
  if (manager_) manager_->close_handles();

  LoadBalancingPolicy::Vec policies = load_balancing_policies();
  for (LoadBalancingPolicy::Vec::const_iterator it = policies.begin();
       it != policies.end(); ++it) {
    (*it)->close_handles();
  }

  is_closing_.store(true);
  async_.send();
}

void RequestProcessor::keyspace_update(const String& keyspace) {
  if (manager_) manager_->set_keyspace(keyspace);
}

void RequestProcessor::init(const ExecutionProfile& default_profile,
                            const ExecutionProfile::Map& profiles,
                            const TokenMap* token_map,
                            bool use_randomized_contact_points,
                            Protected) {
  default_profile_ = default_profile;
  profiles_ = profiles;

  // Build/Assign the load balancing policies from the execution profiles
  default_profile_.build_load_balancing_policy();
  load_balancing_policies_.push_back(default_profile_.load_balancing_policy());
  for (ExecutionProfile::Map::iterator it = profiles_.begin();
       it != profiles_.end(); ++it) {
    it->second.build_load_balancing_policy();
    const LoadBalancingPolicy::Ptr& load_balancing_policy = it->second.load_balancing_policy();
    if (load_balancing_policy) {
      LOG_TRACE("Built load balancing policy for '%s' execution profile",
                it->first.c_str());
      load_balancing_policies_.push_back(load_balancing_policy);
    } else {
      it->second.set_load_balancing_policy(default_profile_.load_balancing_policy().get());
    }
  }

  // Initialize the token map
  internal_token_map_update(token_map);

  // This needs to be done on the control connection thread because it could
  // pause generating a new random seed.
  if (use_randomized_contact_points) {
    random_.reset(Memory::allocate<Random>());
  }
}

void RequestProcessor::connect(const Host::Ptr& current_host,
                               const HostMap& hosts,
                               Metrics* metrics,
                               int protocol_version,
                               const ConnectionPoolManagerSettings& settings,
                               Protected) {
  inc_ref();
  internal_connect(current_host, hosts, metrics, protocol_version, settings);
}

void RequestProcessor::start_async(Protected) {
  async_.start(event_loop_->loop(), this, on_flush);
}

void RequestProcessor::notify_host_add_async(const Host::Ptr& host) {
  event_loop_->add(Memory::allocate<NotifyHostAdd>(host, Ptr(this)));
}

void RequestProcessor::notify_host_remove_async(const Host::Ptr& host) {
  event_loop_->add(Memory::allocate<NotifyHostRemove>(host, Ptr(this)));
}

void RequestProcessor::notify_token_map_update_async(const TokenMap* token_map) {
  event_loop_->add(Memory::allocate<NotifyTokenMapUpdate>(token_map, Ptr(this)));
}

void RequestProcessor::notify_request_async() {
  // Only signal request processing if it's not already processing requests
  if (!is_flushing_.load()) {
    async_.send();
  }
}

void RequestProcessor::on_up(const Address& address) {
  // on_up is using the request processor event loop (no need for a task)
  Host::Ptr host = get_host(address);
  if (host) {
    internal_host_add_down_up(host, Host::UP);
  } else {
    LOG_DEBUG("Tried to up host %s that doesn't exist", address.to_string().c_str());
  }
}

void RequestProcessor::on_down(const Address& address) {
  // on_down cannot be performed asynchronously or memory leak could occur
  Host::Ptr host = get_host(address);
  if (host) {
    internal_host_add_down_up(host, Host::DOWN);
  } else {
    LOG_DEBUG("Tried to down host %s that doesn't exist", address.to_string().c_str());
  }
}

void RequestProcessor::on_critical_error(const Address& address,
                                         Connector::ConnectionError code,
                                         const String& message) {
  on_down(address);
}

void RequestProcessor::on_result_metadata_changed(const String& prepared_id,
                                                  const String& query,
                                                  const String& keyspace,
                                                  const String& result_metadata_id,
                                                  const ResultResponse::ConstPtr& result_response) {
  PreparedMetadata::Entry::Ptr entry(
        Memory::allocate<PreparedMetadata::Entry>(query,
                                                  keyspace,
                                                  result_metadata_id,
                                                  result_response));
  listener_->on_prepared_metadata_update(prepared_id, entry);
}

void RequestProcessor::on_keyspace_changed(const String& keyspace) {
  listener_->on_keyspace_update(keyspace);
}

bool RequestProcessor::on_wait_for_schema_agreement(const RequestHandler::Ptr& request_handler,
                                                    const Host::Ptr& current_host,
                                                    const Response::Ptr& response) {
  SchemaAgreementHandler::Ptr handler(Memory::allocate<SchemaAgreementHandler>(request_handler,
                                                                               current_host,
                                                                               response,
                                                                               this,
                                                                               max_schema_wait_time_ms_));

  PooledConnection::Ptr connection(manager_->find_least_busy(current_host->address()));
  if (connection && connection->write(handler->callback().get())) {
    return true;
  }
  return false;
}

bool RequestProcessor::on_prepare_all(const RequestHandler::Ptr& request_handler,
                                      const Host::Ptr& current_host,
                                      const Response::Ptr& response) {
  if (!prepare_on_all_hosts_) {
    return false;
  }

  AddressVec addresses = manager_->available();
  if (addresses.empty() ||
      (addresses.size() == 1 && addresses[0] == current_host->address())) {
    return false;
  }

  PrepareAllHandler::Ptr prepare_all_handler(Memory::allocate<PrepareAllHandler>(current_host,
                                                                                 response,
                                                                                 request_handler,
                                                                                 // Subtract the node that's already been prepared
                                                                                 addresses.size() - 1));

  for (AddressVec::const_iterator it = addresses.begin(),
       end = addresses.end(); it != end; ++it) {
    const Address& address(*it);

    // Skip over the node we've already prepared
    if (address == current_host->address()) {
      continue;
    }

    // The destructor of `PrepareAllCallback` will decrement the remaining
    // count in `PrepareAllHandler` even if this is unable to write to a
    // connection successfully.
    PrepareAllCallback::Ptr prepare_all_callback(Memory::allocate<PrepareAllCallback>(address,
                                                                                      prepare_all_handler));

    PooledConnection::Ptr connection(manager_->find_least_busy(address));
    if (connection) {
      connection->write(prepare_all_callback.get());
    }
  }

  return true;
}

bool RequestProcessor::on_is_host_up(const Address& address) {
  Host::Ptr host(get_host(address));
  return host && host->is_up();
}

bool RequestProcessor::is_ok() {
  return error_code_ == CASS_OK;
}

void RequestProcessor::internal_connect(const Host::Ptr& current_host,
                                        const HostMap& hosts,
                                        Metrics* metrics,
                                        int protocol_version,
                                        const ConnectionPoolManagerSettings& settings) {
  hosts_ = hosts;

  LoadBalancingPolicy::Vec policies = load_balancing_policies();
  for (LoadBalancingPolicy::Vec::const_iterator it = policies.begin();
       it != policies.end(); ++it) {
    // Initialize the load balancing policies
    (*it)->init(current_host, hosts_, random_.get());
    (*it)->register_handles(event_loop_->loop());
  }

  AddressVec addresses;
  addresses.reserve(hosts_.size());
  std::transform(hosts_.begin(), hosts_.end(), std::back_inserter(addresses), GetAddress());

  ConnectionPoolManagerInitializer::Ptr initializer(Memory::allocate<ConnectionPoolManagerInitializer>(event_loop_,
                                                    protocol_version,
                                                    this,
                                                    on_connection_pool_manager_initialize));
  initializer
    ->with_settings(settings)
    ->with_listener(this)
    ->with_keyspace(connect_keyspace_)
    ->with_metrics(metrics)
    ->initialize(addresses);
}

void RequestProcessor::internal_close() {
  if (manager_) manager_->close();
}

void RequestProcessor::internal_token_map_update(const TokenMap* token_map) {
  token_map_.reset(token_map);
}

Host::Ptr RequestProcessor::get_host(const Address& address) {
  HostMap::iterator it = hosts_.find(address);
  if (it == hosts_.end()) {
    return Host::Ptr();
  }
  return it->second;
}

bool RequestProcessor::execution_profile(const String& name, ExecutionProfile& profile) const {
  // Determine if cluster profile should be used
  if (name.empty()) {
    profile = default_profile_;
    return true;
  }

  // Handle profile lookup
  ExecutionProfile::Map::const_iterator it = profiles_.find(name);
  if (it != profiles_.end()) {
    profile = it->second;
    return true;
  }
  return false;
}

const LoadBalancingPolicy::Vec& RequestProcessor::load_balancing_policies() const {
  return load_balancing_policies_;
}

void RequestProcessor::on_connection_pool_manager_initialize(ConnectionPoolManagerInitializer* initializer) {
  RequestProcessor* request_processor = static_cast<RequestProcessor*>(initializer->data());
  request_processor->internal_connection_pool_manager_initialize(initializer->release_manager(),
                                                                 initializer->failures());
}

void RequestProcessor::internal_connection_pool_manager_initialize(const ConnectionPoolManager::Ptr& manager,
                                                                   const ConnectionPoolConnector::Vec& failures) {
  manager_ = manager;

  // Check for failed connection(s)
  bool is_keyspace_error = false;
  for (ConnectionPoolConnector::Vec::const_iterator it = failures.begin(),
       end = failures.end(); it != end; ++it) {
    ConnectionPoolConnector::Ptr connector(*it);
    if (connector->is_keyspace_error()) {
      is_keyspace_error = true;
      break;
    } else {
      hosts_.erase(connector->address());
    }
  }

  if (is_keyspace_error) {
    error_code_ = CASS_ERROR_LIB_UNABLE_TO_SET_KEYSPACE;
    error_message_ = "Keyspace '" + connect_keyspace_ + "' does not exist";
  } else if (hosts_.empty()) {
    error_code_ = CASS_ERROR_LIB_NO_HOSTS_AVAILABLE;
    error_message_ = "Unable to connect to any hosts";
  } else {
    for (HostMap::iterator it = hosts_.begin(), end = hosts_.end();
         it != end; ++it) {
      it->second->set_up();
    }
  }

  // Notify the callback about the initialization of the request processor
  callback_(this);
  dec_ref();
}

void RequestProcessor::internal_host_add_down_up(const Host::Ptr& host,
                                                 Host::HostState state) {
  if (state == Host::ADDED) {
    manager_->add(host->address());
  }

  bool is_host_ignored = true;
  LoadBalancingPolicy::Vec policies = load_balancing_policies();
  for (LoadBalancingPolicy::Vec::const_iterator it = policies.begin();
       it != policies.end(); ++it) {
    if ((*it)->distance(host) != CASS_HOST_DISTANCE_IGNORE) {
      is_host_ignored = false;
      switch (state) {
        case Host::ADDED:
          (*it)->on_add(host);
          break;
        case Host::DOWN:
          (*it)->on_down(host);
          break;
        case Host::UP:
          (*it)->on_up(host);
          break;
        default:
          assert(false && "Invalid host state");
          break;
      }
    }
  }

  if (is_host_ignored) {
    LOG_DEBUG("Host %s will be ignored by all query plans",
              host->address_string().c_str());
  }
}

void RequestProcessor::internal_host_remove(const Host::Ptr& host) {
  LoadBalancingPolicy::Vec policies = load_balancing_policies();
  for (LoadBalancingPolicy::Vec::const_iterator it = policies.begin();
       it != policies.end(); ++it) {
    (*it)->on_remove(host);
  }
}

void RequestProcessor::on_flush(Async* async) {
  RequestProcessor* request_processor = static_cast<RequestProcessor*>(async->data());
  request_processor->internal_flush_requests();
}

void RequestProcessor::on_flush_timer(Timer* timer) {
  RequestProcessor* request_processor = static_cast<RequestProcessor*>(timer->data());
  request_processor->internal_flush_requests();
}

void RequestProcessor::internal_flush_requests() {
  const int flush_ratio = 90;
  uint64_t start_time_ns = uv_hrtime();

  RequestHandler* request_handler = NULL;
  while (request_queue_->dequeue(request_handler)) {
    if (request_handler) {
      const String& profile_name = request_handler->request()->execution_profile_name();
      ExecutionProfile profile;
      if (execution_profile(profile_name, profile)) {
        if (!profile_name.empty()) {
          LOG_TRACE("Using execution profile '%s'", profile_name.c_str());
        }
        request_handler->init(profile,
                              manager_.get(),
                              token_map_.get(),
                              timestamp_generator_,
                              this);
        request_handler->execute();
      } else {
        request_handler->set_error(CASS_ERROR_LIB_EXECUTION_PROFILE_INVALID,
                                   profile_name + " does not exist");
      }
      request_handler->dec_ref();
    }
  }

  if (is_closing_.load()) {
    async_.close_handle();
    timer_.close_handle();
    return;
  }

  // Determine if a another flush should be scheduled
  is_flushing_.store(false);
  bool expected = false;
  if (request_queue_->is_empty() ||
      !is_flushing_.compare_exchange_strong(expected, true)) {
    return;
  }

  uint64_t flush_time_ns = uv_hrtime() - start_time_ns;
  uint64_t processing_time_ns = flush_time_ns * (100 - flush_ratio) / flush_ratio;
  if (processing_time_ns >= 1000000) { // Schedule another flush to be run in the future
    timer_.start(event_loop_->loop(), (processing_time_ns + 500000) / 1000000, this, on_flush_timer);
  } else {
    async_.send(); // Schedule another flush to be run immediately
  }
}

CassError RequestProcessor::error_code() const {
  return error_code_;
}

String RequestProcessor::error_message() const {
  return error_message_;
}

void* RequestProcessor::data() {
  return data_;
}

void RequestProcessor::NotifyTokenMapUpdate::run(EventLoop* event_loop) {
  request_processor_->internal_token_map_update(token_map_);
}

void RequestProcessor::NotifyHostAdd::run(EventLoop* event_loop) {
  request_processor_->internal_host_add_down_up(host_, Host::ADDED);
}

void RequestProcessor::NotifyHostRemove::run(EventLoop* event_loop) {
  request_processor_->internal_host_remove(host_);
}

} // namespace cass
