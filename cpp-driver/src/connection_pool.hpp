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

#ifndef __CASS_CONNECTION_POOL_HPP_INCLUDED__
#define __CASS_CONNECTION_POOL_HPP_INCLUDED__

#include "address.hpp"
#include "pooled_connection.hpp"
#include "pooled_connector.hpp"

#include <uv.h>

namespace cass {

class ConnectionPoolConnector;
class ConnectionPoolManager;
class EventLoop;

/**
 * A pool of connections to the same host.
 */
class ConnectionPool : public RefCounted<ConnectionPool> {
public:
  typedef SharedRefPtr<ConnectionPool> Ptr;
  typedef DenseHashMap<Address, Ptr, AddressHash> Map;

  /**
   * Constructor. Don't use directly.
   *
   * @param manager The manager for this pool.
   * @param address The address for this pool.
   */
  ConnectionPool(ConnectionPoolManager* manager, const Address& address);
  ~ConnectionPool();

  /**
   * Find the least busy connection for the pool (thread-safe). The least busy
   * connection has the lowest number of outstanding requests.
   *
   * @return The least busy connection or null if no connection is available.
   */
  PooledConnection::Ptr find_least_busy() const;

  /**
   * Close the pool (thread-safe).
   */
  void close();

public:
  ConnectionPoolManager* manager() const { return manager_; }
  const Address& address() const { return  address_; }

public:
  class Protected {
    friend class PooledConnection;
    friend class ConnectionPoolConnector;
    Protected() { }
    Protected(Protected const&) { }
  };

  /**
   * Add connection to the pool.
   *
   * @param connection A connection to add to the pool.
   * @param A key to restrict access to the method.
   */
  void add_connection(const PooledConnection::Ptr& connection, Protected);

  /**
   * Remove the connection and schedule a reconnection.
   *
   * @param connection A connection that closed.
   * @param A key to restrict access to the method.
   */
  void close_connection(PooledConnection* connection, Protected);

  /**
   * Notify the pool manager that the host is up/down or critical.
   *
   * @param connector The connector for this pool.
   * @param A key to restrict access to the method.
   */
  void notify_up_or_down(ConnectionPoolConnector* connector, Protected);

  /**
   * Schedule a new connection.
   *
   * @param event_loop The event loop to use for the reconnection.
   * @param A key to restrict access to the method.
   */
  void schedule_reconnect(EventLoop* event_loop, Protected);

private:
  void internal_add_connection(const PooledConnection::Ptr& connection);
  void internal_schedule_reconnect(EventLoop* event_loop);
  void internal_close(ScopedWriteLock& wl);
  void maybe_closed(ScopedWriteLock& wl);

  static void on_reconnect(PooledConnector* connector, EventLoop* loop);
  void handle_reconnect(PooledConnector* connector, EventLoop* event_loop);

private:
  enum CloseState {
    OPEN,
    CLOSING,
    CLOSED
  };

private:
  ConnectionPoolManager* manager_;
  Address address_;

  mutable uv_rwlock_t rwlock_;
  CloseState close_state_;
  PooledConnection::Vec connections_;
  PooledConnector::Vec pending_connections_;
};

} // namespace cass

#endif
