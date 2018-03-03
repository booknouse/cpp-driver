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

#include "connection_pool_connector.hpp"

#include "connection_pool_manager.hpp"
#include "event_loop.hpp"
#include "memory.hpp"
#include "metrics.hpp"
#include "scoped_lock.hpp"

namespace cass {

ConnectionPoolConnector::ConnectionPoolConnector(ConnectionPoolManager* manager, const Address& address,
                                                 void* data, Callback callback)
  : pool_(Memory::allocate<ConnectionPool>(manager, address))
  , data_(data)
  , callback_(callback)
  , remaining_(0) {
  uv_mutex_init(&lock_);
}

ConnectionPoolConnector::~ConnectionPoolConnector() {
  uv_mutex_destroy(&lock_);
}

void ConnectionPoolConnector::connect(EventLoopGroup* event_loop_group) {
  inc_ref();
  const size_t num_connections_per_host = pool_->manager()->settings().num_connections_per_host;
  remaining_.store(num_connections_per_host);
  for (size_t i = 0; i < num_connections_per_host; ++i) {
    PooledConnector::Ptr connector(Memory::allocate<PooledConnector>(pool_.get(), this, on_connect));
    pending_connections_.push_back(connector);
    connector->connect();
  }
}

void ConnectionPoolConnector::cancel() {
  ScopedMutex l(&lock_);
  pool_->close();
  for (PooledConnector::Vec::iterator it = pending_connections_.begin(),
       end = pending_connections_.end(); it != end; ++it) {
    (*it)->cancel();
  }
}

ConnectionPool::Ptr ConnectionPoolConnector::release_pool() {
  ConnectionPool::Ptr temp = pool_;
  pool_.reset();
  return temp;
}

Connector::ConnectionError ConnectionPoolConnector::error_code() const {
  ScopedMutex l(&lock_);
  return critical_error_connector_ ?  critical_error_connector_->error_code() : Connector::CONNECTION_OK;
}

String ConnectionPoolConnector::error_message() const {
  ScopedMutex l(&lock_);
  return critical_error_connector_ ? critical_error_connector_->error_message() : "";
}

bool ConnectionPoolConnector::is_ok() const {
  return !is_critical_error();
}

bool ConnectionPoolConnector::is_critical_error() const {
  ScopedMutex l(&lock_);
  return critical_error_connector_;
}

bool ConnectionPoolConnector::is_keyspace_error() const {
  ScopedMutex l(&lock_);
  if (critical_error_connector_) {
    return critical_error_connector_->is_keyspace_error();
  }
  return false;
}

void ConnectionPoolConnector::on_connect(PooledConnector* connector, EventLoop* event_loop) {
  ConnectionPoolConnector* pool_connector = static_cast<ConnectionPoolConnector*>(connector->data());
  pool_connector->handle_connect(connector, event_loop);
}

void ConnectionPoolConnector::handle_connect(PooledConnector* connector, EventLoop* event_loop) {
  { // Locked
    ScopedMutex l(&lock_);
    pending_connections_.erase(std::remove(pending_connections_.begin(), pending_connections_.end(), connector),
                               pending_connections_.end());

    if (connector->is_ok()) {
      pool_->add_connection(connector->release_connection(), ConnectionPool::Protected());
    } else if (!connector->is_cancelled()){
      LOG_ERROR("Connection pool was unable to connect to host %s because of the following error: %s",
                pool_->address().to_string().c_str(),
                connector->error_message().c_str());

      if (connector->is_critical_error())  {
        if (!critical_error_connector_) {
          critical_error_connector_.reset(connector);
          pool_->close();
          for (PooledConnector::Vec::iterator it = pending_connections_.begin(),
               end = pending_connections_.end(); it != end; ++it) {
            (*it)->cancel();
          }
        }
      } else {
        pool_->schedule_reconnect(event_loop, ConnectionPool::Protected());
      }
    }
  }

  if (remaining_.fetch_sub(1) - 1 == 0) {
    pool_->notify_up_or_down(this, ConnectionPool::Protected());
    callback_(this);
    // If the pool hasn't been released then close it.
    if (pool_) pool_->close();
    dec_ref();
  }
}

} // namespace cass
