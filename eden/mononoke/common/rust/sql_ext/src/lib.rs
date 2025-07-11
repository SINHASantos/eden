/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This software may be used and distributed according to the terms of the
 * GNU General Public License version 2.
 */

mod mononoke_queries;
#[cfg(not(fbcode_build))]
mod oss;
pub mod replication;
mod sqlite;
mod telemetry;
#[cfg(test)]
mod tests;

use anyhow::Result;
use sql::Connection;
use sql::QueryTelemetry;
pub use sql::SqlConnections;
pub use sql::SqlShardedConnections;
use sql::Transaction as SqlTransaction;
pub use sql_query_telemetry::SqlQueryTelemetry;
pub use sqlite::open_existing_sqlite_path;
pub use sqlite::open_sqlite_in_memory;
pub use sqlite::open_sqlite_path;

pub use crate::mononoke_queries::should_retry_mysql_query as should_retry_query;

#[must_use]
pub enum TransactionResult {
    Succeeded(Transaction),
    Failed,
}

pub mod _macro_internal {
    pub use std::collections::hash_map::DefaultHasher;
    pub use std::hash::Hash;
    pub use std::hash::Hasher;
    pub use std::sync::Arc;

    pub use anyhow::Result;
    pub use clientinfo::ClientEntryPoint;
    pub use clientinfo::ClientRequestInfo;
    pub use mononoke_types::RepositoryId;
    pub use paste;
    pub use serde_json;
    pub use sql::Connection;
    pub use sql::WriteResult;
    pub use sql::queries;
    pub use sql_query_config::SqlQueryConfig;
    pub use sql_query_telemetry::SqlQueryTelemetry;
    pub use twox_hash::xxh3::Hash128;
    pub use twox_hash::xxh3::HasherExt;

    pub use crate::Transaction;
    pub use crate::mononoke_queries::CacheData;
    pub use crate::mononoke_queries::CachedQueryResult;
    pub use crate::mononoke_queries::query_with_retry;
    pub use crate::mononoke_queries::query_with_retry_no_cache;
    pub use crate::telemetry::TelemetryGranularity;
    pub use crate::telemetry::log_query_error;
    pub use crate::telemetry::log_query_telemetry;
}

/// Wrapper over the SQL transaction that will keep track of telemetry from the
/// entire transaction.
pub struct Transaction {
    pub inner: SqlTransaction,
}

impl Transaction {
    /// Create a new transaction for the provided connection.
    pub async fn new(connection: &Connection) -> Result<Self> {
        let inner = SqlTransaction::new(connection).await?;
        Ok(Self { inner })
    }

    /// Create a new transaction for the provided connection.
    pub fn from_sql_transaction(sql_txn: SqlTransaction) -> Self {
        Self { inner: sql_txn }
    }

    /// Perform a commit on this transaction
    pub async fn commit(self) -> Result<()> {
        self.inner.commit().await
    }

    /// Perform a rollback on this transaction
    pub async fn rollback(self) -> Result<()> {
        self.inner.rollback().await
    }
}

impl From<SqlTransaction> for Transaction {
    fn from(sql_txn: SqlTransaction) -> Self {
        Self { inner: sql_txn }
    }
}

pub mod facebook {
    #[cfg(fbcode_build)]
    mod r#impl;

    use std::fmt;
    use std::fmt::Debug;

    #[cfg(fbcode_build)]
    pub use r#impl::PoolConfig;
    #[cfg(fbcode_build)]
    pub use r#impl::SharedConnectionPool;
    #[cfg(fbcode_build)]
    pub use r#impl::create_mysql_connections_sharded;
    #[cfg(fbcode_build)]
    pub use r#impl::create_mysql_connections_unsharded;
    #[cfg(fbcode_build)]
    pub use r#impl::create_oss_mysql_connections_unsharded;
    #[cfg(fbcode_build)]
    pub use r#impl::myadmin::MyAdmin;
    #[cfg(fbcode_build)]
    pub use r#impl::myadmin::MyAdminLagMonitor;
    #[cfg(fbcode_build)]
    pub use r#impl::myadmin::replication_status_chunked;

    #[cfg(not(fbcode_build))]
    pub use crate::oss::MyAdmin;
    #[cfg(not(fbcode_build))]
    pub use crate::oss::MyAdminLagMonitor;
    #[cfg(not(fbcode_build))]
    pub use crate::oss::PoolConfig;
    #[cfg(not(fbcode_build))]
    pub use crate::oss::SharedConnectionPool;
    #[cfg(not(fbcode_build))]
    pub use crate::oss::create_mysql_connections_sharded;
    #[cfg(not(fbcode_build))]
    pub use crate::oss::create_mysql_connections_unsharded;

    /// MySQL global shared connection pool configuration.
    #[derive(Clone, Default)]
    pub struct MysqlOptions {
        pub pool: SharedConnectionPool,
        // pool config is used only once when the shared connection pool is being created
        pub pool_config: PoolConfig,
        pub read_connection_type: ReadConnectionType,
    }

    impl MysqlOptions {
        pub fn per_key_limit(&self) -> Option<usize> {
            #[cfg(not(fbcode_build))]
            {
                None
            }
            #[cfg(fbcode_build)]
            {
                Some(self.pool_config.per_key_limit as usize)
            }
        }
    }

    impl Debug for MysqlOptions {
        fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
            write!(
                f,
                "MySQL pool with config {:?}, connection type: {:?}",
                self.pool_config, self.read_connection_type
            )
        }
    }

    /// Mirrors facebook::db::InstanceRequirement enum for DBLocator
    #[derive(Copy, Clone, Debug, Default)]
    pub enum ReadConnectionType {
        /// Choose master or replica, whatever is closest and available.
        /// Use this if both master and replica are in the same region, and reads
        /// should we served by both.
        Closest,
        /// Choose replicas only, avoiding the master, even if it means going to a
        /// remote region.
        #[default]
        ReplicaOnly,
        /// Choose master only (typically for writes). Will never connect to replica.
        Master,
        /// Choose closer first and inside the same region, replicas first.
        /// In case both master and replica in the same region - all reads
        /// will be routed to the replica.
        ReplicaFirst,
        /// Choose replicas that satisfy a lower bound HLC value in order to
        /// perform consistent read-your-writes operations
        ReadAfterWriteConsistency,
    }
}
