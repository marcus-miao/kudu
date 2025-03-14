// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.
#include "kudu/tablet/tablet_metrics.h"

#include <functional>
#include <map>
#include <utility>
#include <cstdint>

#include "kudu/tablet/rowset.h"
#include "kudu/util/memory/arena.h"
#include "kudu/util/metrics.h"
#include "kudu/util/trace.h"

// Tablet-specific metrics.
METRIC_DEFINE_counter(tablet, rows_inserted, "Rows Inserted",
    kudu::MetricUnit::kRows,
    "Number of rows inserted into this tablet since service start",
    kudu::MetricLevel::kInfo);
METRIC_DEFINE_counter(tablet, insert_ignore_errors, "Insert Ignore Errors",
    kudu::MetricUnit::kRows,
    "Number of insert ignore operations for this tablet which were "
    "ignored due to an error since service start",
    kudu::MetricLevel::kDebug);
METRIC_DEFINE_counter(tablet, rows_upserted, "Rows Upserted",
    kudu::MetricUnit::kRows,
    "Number of rows upserted into this tablet since service start",
    kudu::MetricLevel::kInfo);
METRIC_DEFINE_counter(tablet, upsert_ignore_errors, "Upsert Ignore Errors",
                      kudu::MetricUnit::kRows,
                      "Number of upsert ignore operations for this tablet which were "
                      "ignored due to an error since service start. This metric counts "
                      "the number of attempts to update a present row by changing the "
                      "value of any of its immutable cells. Note that the rest of the "
                      "cells (i.e. the mutable ones) in such case are updated accordingly "
                      "to the operation's data，and rows_upserted will be counted too if "
                      "upsert successfully.",
                      kudu::MetricLevel::kDebug);
METRIC_DEFINE_counter(tablet, rows_updated, "Rows Updated",
    kudu::MetricUnit::kRows,
    "Number of row update operations performed on this tablet since service start",
    kudu::MetricLevel::kInfo);
METRIC_DEFINE_counter(tablet, update_ignore_errors, "Update Ignore Errors",
                      kudu::MetricUnit::kRows,
                      "Number of update ignore operations for this tablet which were "
                      "ignored due to an error since service start. Note that when "
                      "ignoring to update the immutable cells, the rest of the cells "
                      "(i.e. the mutable ones) in such case are updated accordingly to "
                      "the operation's data，and rows_updated will be counted too if "
                      "update successfully.",
                      kudu::MetricLevel::kDebug);
METRIC_DEFINE_counter(tablet, rows_deleted, "Rows Deleted",
    kudu::MetricUnit::kRows,
    "Number of row delete operations performed on this tablet since service start",
    kudu::MetricLevel::kInfo);
METRIC_DEFINE_counter(tablet, delete_ignore_errors, "Delete Ignore Errors",
                      kudu::MetricUnit::kRows,
                      "Number of delete ignore operations for this tablet which were "
                      "ignored due to an error since service start",
                      kudu::MetricLevel::kDebug);

METRIC_DEFINE_counter(tablet, insertions_failed_dup_key, "Duplicate Key Inserts",
                      kudu::MetricUnit::kRows,
                      "Number of inserts which failed because the key already existed",
                      kudu::MetricLevel::kDebug);

METRIC_DEFINE_counter(tablet, upserts_as_updates, "Upserts converted into updates",
                      kudu::MetricUnit::kRows,
                      "Number of upserts which were applied as updates because the key already "
                      "existed.",
                      kudu::MetricLevel::kInfo);

METRIC_DEFINE_counter(tablet, scanner_rows_returned, "Scanner Rows Returned",
                      kudu::MetricUnit::kRows,
                      "Number of rows returned by scanners to clients. This count "
                      "is measured after predicates are applied, and thus is not "
                      "a reflection of the amount of work being done by scanners.",
                      kudu::MetricLevel::kInfo);
METRIC_DEFINE_counter(tablet, scanner_cells_returned, "Scanner Cells Returned",
                      kudu::MetricUnit::kCells,
                      "Number of table cells returned by scanners to clients. This count "
                      "is measured after predicates are applied, and thus is not "
                      "a reflection of the amount of work being done by scanners.",
                      kudu::MetricLevel::kDebug);
METRIC_DEFINE_counter(tablet, scanner_bytes_returned, "Scanner Bytes Returned",
                      kudu::MetricUnit::kBytes,
                      "Number of bytes returned by scanners to clients. This count "
                      "is measured after predicates are applied and the data is decoded "
                      "for consumption by clients, and thus is not "
                      "a reflection of the amount of work being done by scanners.",
                      kudu::MetricLevel::kDebug);
METRIC_DEFINE_counter(tablet, scanner_predicates_disabled, "Scanner Column Predicates Disabled",
                      kudu::MetricUnit::kUnits,
                      "Number of column predicates disabled during scan requests. "
                      "This count measures the number of disableable column predicates like "
                      "Bloom filter predicate that are automatically disabled if determined to "
                      "be ineffective.",
                      kudu::MetricLevel::kDebug);

METRIC_DEFINE_counter(tablet, scanner_rows_scanned, "Scanner Rows Scanned",
                      kudu::MetricUnit::kRows,
                      "Number of rows processed by scan requests. This is measured "
                      "as a raw count prior to application of predicates, deleted data,"
                      "or MVCC-based filtering. Thus, this is a better measure of actual "
                      "table rows that have been processed by scan operations compared "
                      "to the Scanner Rows Returned metric.",
                      kudu::MetricLevel::kInfo);

METRIC_DEFINE_counter(tablet, scanner_cells_scanned_from_disk, "Scanner Cells Scanned From Disk",
                      kudu::MetricUnit::kCells,
                      "Number of table cells processed by scan requests. This is measured "
                      "as a raw count prior to application of predicates, deleted data,"
                      "or MVCC-based filtering. Thus, this is a better measure of actual "
                      "table cells that have been processed by scan operations compared "
                      "to the Scanner Cells Returned metric. "
                      "Note that this only counts data that has been flushed to disk, "
                      "and does not include data read from in-memory stores. However, it "
                      "includes both cache misses and cache hits.",
                      kudu::MetricLevel::kDebug);

METRIC_DEFINE_counter(tablet, scanner_bytes_scanned_from_disk, "Scanner Bytes Scanned From Disk",
                      kudu::MetricUnit::kBytes,
                      "Number of bytes read by scan requests. This is measured "
                      "as a raw count prior to application of predicates, deleted data,"
                      "or MVCC-based filtering. Thus, this is a better measure of actual "
                      "IO that has been caused by scan operations compared "
                      "to the Scanner Bytes Returned metric. "
                      "Note that this only counts data that has been flushed to disk, "
                      "and does not include data read from in-memory stores. However, it "
                      "includes both cache misses and cache hits.",
                      kudu::MetricLevel::kDebug);

METRIC_DEFINE_counter(tablet, scans_started, "Scans Started",
                      kudu::MetricUnit::kScanners,
                      "Number of scanners which have been started on this tablet",
                      kudu::MetricLevel::kInfo);
METRIC_DEFINE_gauge_size(tablet, tablet_active_scanners, "Active Scanners",
                         kudu::MetricUnit::kScanners,
                         "Number of scanners that are currently active on this tablet",
                         kudu::MetricLevel::kInfo);

METRIC_DEFINE_histogram(tablet, scan_duration_wall_time,
                        "Scan Requests Wall Time",
                        kudu::MetricUnit::kMilliseconds,
                        "Duration of scan requests, wall time.",
                        kudu::MetricLevel::kDebug,
                        60000LU, 1);

METRIC_DEFINE_histogram(tablet, scan_duration_system_time,
                        "Scan Requests System Time",
                        kudu::MetricUnit::kMilliseconds,
                        "Duration of scan requests, system time.",
                        kudu::MetricLevel::kDebug,
                        60000LU, 1);

METRIC_DEFINE_histogram(tablet, scan_duration_user_time,
                        "Scan Requests User Time",
                        kudu::MetricUnit::kMilliseconds,
                        "Duration of scan requests, user time.",
                        kudu::MetricLevel::kDebug,
                        60000LU, 1);

METRIC_DEFINE_counter(tablet, bloom_lookups, "Bloom Filter Lookups",
                      kudu::MetricUnit::kProbes,
                      "Number of times a bloom filter was consulted",
                      kudu::MetricLevel::kDebug);
METRIC_DEFINE_counter(tablet, key_file_lookups, "Key File Lookups",
                      kudu::MetricUnit::kProbes,
                      "Number of times a key cfile was consulted",
                      kudu::MetricLevel::kDebug);
METRIC_DEFINE_counter(tablet, delta_file_lookups, "Delta File Lookups",
                      kudu::MetricUnit::kProbes,
                      "Number of times a delta file was consulted",
                      kudu::MetricLevel::kDebug);
METRIC_DEFINE_counter(tablet, mrs_lookups, "MemRowSet Lookups",
                      kudu::MetricUnit::kProbes,
                      "Number of times a MemRowSet was consulted.",
                      kudu::MetricLevel::kDebug);
METRIC_DEFINE_counter(tablet, bytes_flushed, "Bytes Flushed",
                      kudu::MetricUnit::kBytes,
                      "Amount of data that has been flushed to disk by this tablet.",
                      kudu::MetricLevel::kDebug);

METRIC_DEFINE_counter(tablet, undo_delta_block_gc_bytes_deleted,
                      "Undo Delta Block GC Bytes Deleted",
                      kudu::MetricUnit::kBytes,
                      "Number of bytes deleted by garbage-collecting old UNDO delta blocks "
                      "on this tablet since this server was restarted. "
                      "Does not include bytes garbage collected during compactions.",
                      kudu::MetricLevel::kDebug);

METRIC_DEFINE_counter(tablet, deleted_rowset_gc_bytes_deleted,
                      "Deleted Rowsets GC Bytes Deleted",
                      kudu::MetricUnit::kBytes,
                      "Number of bytes deleted by garbage-collecting deleted rowsets.",
                      kudu::MetricLevel::kDebug);

METRIC_DEFINE_counter(tablet, ops_timed_out_in_prepare_queue,
                      "Number of Requests Timed Out In Prepare Queue",
                      kudu::MetricUnit::kRequests,
                      "Number of WriteRequest RPCs that timed out while their "
                      "corresponding operations were waiting in the tablet's "
                      "prepare queue, and thus were not started but "
                      "acknowledged with TimedOut error status.",
                      kudu::MetricLevel::kWarn);

METRIC_DEFINE_histogram(tablet, bloom_lookups_per_op, "Bloom Lookups per Operation",
                        kudu::MetricUnit::kProbes,
                        "Tracks the number of bloom filter lookups performed by each "
                        "operation. A single operation may perform several bloom filter "
                        "lookups if the tablet is not fully compacted. High frequency of "
                        "high values may indicate that compaction is falling behind.",
                        kudu::MetricLevel::kDebug,
                        20, 2);

METRIC_DEFINE_histogram(tablet, key_file_lookups_per_op, "Key Lookups per Operation",
                        kudu::MetricUnit::kProbes,
                        "Tracks the number of key file lookups performed by each "
                        "operation. A single operation may perform several key file "
                        "lookups if the tablet is not fully compacted and if bloom filters "
                        "are not effectively culling lookups.",
                        kudu::MetricLevel::kDebug,
                        20, 2);

METRIC_DEFINE_histogram(tablet, delta_file_lookups_per_op, "Delta File Lookups per Operation",
                        kudu::MetricUnit::kProbes,
                        "Tracks the number of delta file lookups performed by each "
                        "operation. A single operation may perform several delta file "
                        "lookups if the tablet is not fully compacted. High frequency of "
                        "high values may indicate that compaction is falling behind.",
                        kudu::MetricLevel::kDebug,
                        20, 2);

METRIC_DEFINE_histogram(tablet, alter_schema_duration,
                        "Alter Schema Op Duration",
                        kudu::MetricUnit::kMicroseconds,
                        "Duration of alter schema ops to this tablet.",
                        kudu::MetricLevel::kDebug,
                        60000000LU, 2);

METRIC_DEFINE_histogram(tablet, replication_duration,
                        "Replica Replication Duration",
                        kudu::MetricUnit::kMicroseconds,
                        "Duration of replication between replicas on the leader.",
                        kudu::MetricLevel::kDebug,
                        60000000LU, 2);

METRIC_DEFINE_histogram(tablet, write_op_duration_client_propagated_consistency,
  "Write Op Duration with Propagated Consistency",
  kudu::MetricUnit::kMicroseconds,
  "Duration of writes to this tablet with external consistency set to CLIENT_PROPAGATED.",
  kudu::MetricLevel::kDebug,
  60000000LU, 2);

METRIC_DEFINE_histogram(tablet, write_op_duration_commit_wait_consistency,
  "Write Op Duration with Commit-Wait Consistency",
  kudu::MetricUnit::kMicroseconds,
  "Duration of writes to this tablet with external consistency set to COMMIT_WAIT.",
  kudu::MetricLevel::kDebug,
  60000000LU, 2);

METRIC_DEFINE_histogram(tablet, commit_wait_duration,
  "Commit-Wait Duration",
  kudu::MetricUnit::kMicroseconds,
  "Time spent waiting for COMMIT_WAIT external consistency writes for this tablet.",
  kudu::MetricLevel::kDebug,
  60000000LU, 2);

METRIC_DEFINE_histogram(tablet, snapshot_read_inflight_wait_duration,
  "Time Waiting For Snapshot Reads",
  kudu::MetricUnit::kMicroseconds,
  "Time spent waiting for in-flight writes to complete for READ_AT_SNAPSHOT scans.",
  kudu::MetricLevel::kDebug,
  60000000LU, 2);

METRIC_DEFINE_gauge_uint32(tablet, flush_dms_running,
  "DeltaMemStore Flushes Running",
  kudu::MetricUnit::kMaintenanceOperations,
  "Number of delta memstore flushes currently running.",
  kudu::MetricLevel::kDebug);

METRIC_DEFINE_gauge_uint32(tablet, flush_mrs_running,
  "MemRowSet Flushes Running",
  kudu::MetricUnit::kMaintenanceOperations,
  "Number of MemRowSet flushes currently running.",
  kudu::MetricLevel::kDebug);

METRIC_DEFINE_gauge_uint32(tablet, compact_rs_running,
  "RowSet Compactions Running",
  kudu::MetricUnit::kMaintenanceOperations,
  "Number of RowSet compactions currently running.",
  kudu::MetricLevel::kDebug);

METRIC_DEFINE_gauge_uint32(tablet, delta_minor_compact_rs_running,
  "Minor Delta Compactions Running",
  kudu::MetricUnit::kMaintenanceOperations,
  "Number of delta minor compactions currently running.",
  kudu::MetricLevel::kDebug);

METRIC_DEFINE_gauge_uint32(tablet, delta_major_compact_rs_running,
  "Major Delta Compactions Running",
  kudu::MetricUnit::kMaintenanceOperations,
  "Number of delta major compactions currently running.",
  kudu::MetricLevel::kDebug);

METRIC_DEFINE_gauge_uint32(tablet, undo_delta_block_gc_running,
  "Undo Delta Block GC Running",
  kudu::MetricUnit::kMaintenanceOperations,
  "Number of UNDO delta block GC operations currently running.",
  kudu::MetricLevel::kDebug);

METRIC_DEFINE_gauge_int64(tablet, undo_delta_block_estimated_retained_bytes,
  "Estimated Deletable Bytes Retained in Undo Delta Blocks",
  kudu::MetricUnit::kBytes,
  "Estimated bytes of deletable data in undo delta blocks for this tablet. "
  "May be an overestimate.",
  kudu::MetricLevel::kDebug);

METRIC_DEFINE_gauge_uint32(tablet, deleted_rowset_gc_running,
  "Deleted Rowset GC Running",
  kudu::MetricUnit::kMaintenanceOperations,
  "Number of deleted rowset GC operations currently running.",
  kudu::MetricLevel::kDebug);

METRIC_DEFINE_gauge_int64(tablet, deleted_rowset_estimated_retained_bytes,
  "Estimated Deletable Bytes Retained in Deleted Rowsets",
  kudu::MetricUnit::kBytes,
  "Estimated bytes of deletable data in deleted rowsets for this tablet.",
  kudu::MetricLevel::kDebug);

METRIC_DEFINE_histogram(tablet, flush_dms_duration,
  "DeltaMemStore Flush Duration",
  kudu::MetricUnit::kMilliseconds,
  "Time spent flushing DeltaMemStores.",
  kudu::MetricLevel::kDebug,
  60000LU, 1);

METRIC_DEFINE_histogram(tablet, flush_mrs_duration,
  "MemRowSet Flush Duration",
  kudu::MetricUnit::kMilliseconds,
  "Time spent flushing MemRowSets.",
  kudu::MetricLevel::kDebug,
  60000LU, 1);

METRIC_DEFINE_histogram(tablet, compact_rs_duration,
  "RowSet Compaction Duration",
  kudu::MetricUnit::kMilliseconds,
  "Time spent compacting RowSets.",
  kudu::MetricLevel::kDebug,
  60000LU, 1);

METRIC_DEFINE_histogram(tablet, delta_minor_compact_rs_duration,
  "Minor Delta Compaction Duration",
  kudu::MetricUnit::kMilliseconds,
  "Time spent minor delta compacting.",
  kudu::MetricLevel::kDebug,
  60000LU, 1);

METRIC_DEFINE_histogram(tablet, delta_major_compact_rs_duration,
  "Major Delta Compaction Duration",
  kudu::MetricUnit::kSeconds,
  "Seconds spent major delta compacting.",
  kudu::MetricLevel::kDebug,
  60000000LU, 2);

METRIC_DEFINE_histogram(tablet, undo_delta_block_gc_init_duration,
  "Undo Delta Block GC Init Duration",
  kudu::MetricUnit::kMilliseconds,
  "Time spent initializing ancient UNDO delta blocks.",
  kudu::MetricLevel::kDebug,
  60000LU, 1);

METRIC_DEFINE_histogram(tablet, undo_delta_block_gc_delete_duration,
  "Undo Delta Block GC Delete Duration",
  kudu::MetricUnit::kMilliseconds,
  "Time spent deleting ancient UNDO delta blocks.",
  kudu::MetricLevel::kDebug,
  60000LU, 1);

METRIC_DEFINE_histogram(tablet, undo_delta_block_gc_perform_duration,
  "Undo Delta Block GC Perform Duration",
  kudu::MetricUnit::kMilliseconds,
  "Time spent running the maintenance operation to GC ancient UNDO delta blocks.",
  kudu::MetricLevel::kDebug,
  60000LU, 1);

METRIC_DEFINE_histogram(tablet, compact_rs_mem_usage,
  "Peak Memory Usage for CompactRowSetsOp",
  kudu::MetricUnit::kBytes,
  "Peak memory usage of rowset merge compaction operations (CompactRowSetsOp)",
  kudu::MetricLevel::kDebug,
  60000LU, 1);

METRIC_DEFINE_histogram(tablet, compact_rs_mem_usage_to_deltas_size_ratio,
  "Peak Memory Usage to On-Disk Delta Size Ratio for CompactRowSetsOp",
  kudu::MetricUnit::kUnits,
  "Ratio of the peak memory usage to the estimated on-disk size of all deltas "
  "for rowsets involved in rowset merge compaction (CompactRowSetsOp)",
  kudu::MetricLevel::kDebug,
  60000LU, 1);

METRIC_DEFINE_histogram(tablet, deleted_rowset_gc_duration,
  "Deleted Rowset GC Duration",
  kudu::MetricUnit::kMilliseconds,
  "Time spent running the maintenance operation to GC deleted rowsets.",
  kudu::MetricLevel::kDebug,
  60000LU, 1);

METRIC_DEFINE_counter(tablet, leader_memory_pressure_rejections,
  "Leader Memory Pressure Rejections",
  kudu::MetricUnit::kRequests,
  "Number of RPC requests rejected due to memory pressure while LEADER.",
  kudu::MetricLevel::kWarn);

METRIC_DEFINE_gauge_double(tablet, average_diskrowset_height, "Average DiskRowSet Height",
                           kudu::MetricUnit::kUnits,
                           "Average height of the diskrowsets in this tablet "
                           "replica. The larger the average height, the more "
                           "uncompacted the tablet replica is.",
                           kudu::MetricLevel::kInfo);
METRIC_DECLARE_gauge_size(merged_entities_count_of_tablet);

namespace kudu {
namespace tablet {

#define MINIT(x) x(METRIC_##x.Instantiate(entity))
#define GINIT(x) x(METRIC_##x.Instantiate(entity, 0))
#define MEANINIT(x) x(METRIC_##x.InstantiateMeanGauge(entity))
#define HIDEINIT(x, v) x(METRIC_##x.InstantiateHidden(entity, v))
TabletMetrics::TabletMetrics(const scoped_refptr<MetricEntity>& entity)
  : MINIT(rows_inserted),
    MINIT(rows_upserted),
    MINIT(rows_updated),
    MINIT(rows_deleted),
    MINIT(insert_ignore_errors),
    MINIT(upsert_ignore_errors),
    MINIT(update_ignore_errors),
    MINIT(delete_ignore_errors),
    MINIT(insertions_failed_dup_key),
    MINIT(upserts_as_updates),
    MINIT(scanner_rows_returned),
    MINIT(scanner_cells_returned),
    MINIT(scanner_bytes_returned),
    MINIT(scanner_rows_scanned),
    MINIT(scanner_cells_scanned_from_disk),
    MINIT(scanner_bytes_scanned_from_disk),
    MINIT(scanner_predicates_disabled),
    MINIT(scans_started),
    GINIT(tablet_active_scanners),
    MINIT(scan_duration_wall_time),
    MINIT(scan_duration_system_time),
    MINIT(scan_duration_user_time),
    MINIT(bloom_lookups),
    MINIT(key_file_lookups),
    MINIT(delta_file_lookups),
    MINIT(mrs_lookups),
    MINIT(bytes_flushed),
    MINIT(deleted_rowset_gc_bytes_deleted),
    MINIT(undo_delta_block_gc_bytes_deleted),
    MINIT(ops_timed_out_in_prepare_queue),
    MINIT(bloom_lookups_per_op),
    MINIT(key_file_lookups_per_op),
    MINIT(delta_file_lookups_per_op),
    MINIT(commit_wait_duration),
    MINIT(snapshot_read_inflight_wait_duration),
    MINIT(write_op_duration_client_propagated_consistency),
    MINIT(write_op_duration_commit_wait_consistency),
    MINIT(alter_schema_duration),
    MINIT(replication_duration),
    GINIT(flush_dms_running),
    GINIT(flush_mrs_running),
    GINIT(compact_rs_running),
    GINIT(deleted_rowset_estimated_retained_bytes),
    GINIT(deleted_rowset_gc_running),
    GINIT(delta_minor_compact_rs_running),
    GINIT(delta_major_compact_rs_running),
    GINIT(undo_delta_block_gc_running),
    GINIT(undo_delta_block_estimated_retained_bytes),
    MINIT(flush_dms_duration),
    MINIT(flush_mrs_duration),
    MINIT(compact_rs_duration),
    MINIT(deleted_rowset_gc_duration),
    MINIT(delta_minor_compact_rs_duration),
    MINIT(delta_major_compact_rs_duration),
    MINIT(undo_delta_block_gc_init_duration),
    MINIT(undo_delta_block_gc_delete_duration),
    MINIT(undo_delta_block_gc_perform_duration),
    MINIT(compact_rs_mem_usage),
    MINIT(compact_rs_mem_usage_to_deltas_size_ratio),
    MINIT(leader_memory_pressure_rejections),
    MEANINIT(average_diskrowset_height),
    HIDEINIT(merged_entities_count_of_tablet, 1) {
}
#undef MINIT
#undef GINIT
#undef MEANINIT
#undef HIDEINIT

void TabletMetrics::AddProbeStats(const ProbeStats* stats_array, int len,
                                  Arena* work_arena) {
  // In most cases, different operations within a batch will have the same
  // statistics (e.g. 1 or 2 bloom lookups, 0 key lookups, 0 delta lookups).
  //
  // Given that, we pre-aggregate our contributions to the tablet histograms
  // in local maps here. We also pre-aggregate our normal counter contributions
  // to minimize contention on the counter metrics.
  //
  // To avoid any actual expensive allocation, we allocate these local maps from
  // 'work_arena'.
  typedef ArenaAllocator<std::pair<const int32_t, int32_t>, false> AllocType;
  typedef std::map<int32_t, int32_t, std::less<int32_t>, AllocType> MapType;
  AllocType alloc(work_arena);
  MapType bloom_lookups_hist(std::less<int32_t>(), alloc);
  MapType key_file_lookups_hist(std::less<int32_t>(), alloc);
  MapType delta_file_lookups_hist(std::less<int32_t>(), alloc);

  ProbeStats sum;
  for (int i = 0; i < len; i++) {
    const ProbeStats& stats = stats_array[i];

    sum.blooms_consulted += stats.blooms_consulted;
    sum.keys_consulted += stats.keys_consulted;
    sum.deltas_consulted += stats.deltas_consulted;
    sum.mrs_consulted += stats.mrs_consulted;

    bloom_lookups_hist[stats.blooms_consulted]++;
    key_file_lookups_hist[stats.keys_consulted]++;
    delta_file_lookups_hist[stats.deltas_consulted]++;
  }

  bloom_lookups->IncrementBy(sum.blooms_consulted);
  key_file_lookups->IncrementBy(sum.keys_consulted);
  delta_file_lookups->IncrementBy(sum.deltas_consulted);
  mrs_lookups->IncrementBy(sum.mrs_consulted);

  for (const auto& entry : bloom_lookups_hist) {
    bloom_lookups_per_op->IncrementBy(entry.first, entry.second);
  }
  for (const auto& entry : key_file_lookups_hist) {
    key_file_lookups_per_op->IncrementBy(entry.first, entry.second);
  }
  for (const auto& entry : delta_file_lookups_hist) {
    delta_file_lookups_per_op->IncrementBy(entry.first, entry.second);
  }

  TRACE("ProbeStats: bloom_lookups=$0,key_file_lookups=$1,"
        "delta_file_lookups=$2,mrs_lookups=$3",
        sum.blooms_consulted, sum.keys_consulted,
        sum.deltas_consulted, sum.mrs_consulted);
}

} // namespace tablet
} // namespace kudu
