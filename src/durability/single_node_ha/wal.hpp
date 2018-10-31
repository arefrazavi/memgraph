#pragma once

#include <chrono>
#include <cstdint>
#include <experimental/filesystem>

#include <gflags/gflags.h>
#include <glog/logging.h>

#include "communication/bolt/v1/encoder/base_encoder.hpp"
#include "data_structures/ring_buffer.hpp"
#include "durability/single_node_ha/state_delta.hpp"
#include "storage/common/property_value.hpp"
#include "storage/common/types.hpp"
#include "storage/single_node_ha/gid.hpp"
#include "transactions/type.hpp"
#include "utils/scheduler.hpp"

namespace durability {

/// A database StateDelta log for durability. Buffers and periodically
/// serializes small-granulation database deltas (StateDelta).
///
/// The order is not deterministic in a multithreaded scenario (multiple DB
/// transactions). This is fine, the recovery process should be immune to this
/// indeterminism.
class WriteAheadLog {
 public:
  WriteAheadLog(const std::experimental::filesystem::path &durability_dir,
                bool durability_enabled, bool synchronous_commit);
  ~WriteAheadLog();

  ///  Initializes the WAL. Called at the end of GraphDb construction, after
  /// (optional) recovery. Also responsible for initializing the wal_file.
  void Init();

  /// Emplaces the given DeltaState onto the buffer, if the WAL is enabled.
  /// If the WAL is configured to work in synchronous commit mode, emplace will
  /// flush the buffers if a delta represents a transaction end.
  void Emplace(const database::StateDelta &delta);

  /// Flushes every delta currently in the ring buffer.
  /// This method should only be called from tests.
  void Flush();

 private:
  /// Groups the logic of WAL file handling (flushing, naming, rotating)
  class WalFile {
   public:
    explicit WalFile(const std::experimental::filesystem::path &durability_dir);
    ~WalFile();

    /// Initializes the WAL file. Must be called before first flush. Can be
    /// called after Flush() to re-initialize stuff.
    void Init();

    /// Flushes all the deltas in the buffer to the WAL file. If necessary
    /// rotates the file.
    void Flush(RingBuffer<database::StateDelta> &buffer);

   private:
    /// Mutex used for flushing wal data
    std::mutex flush_mutex_;
    const std::experimental::filesystem::path wal_dir_;
    HashedFileWriter writer_;
    communication::bolt::BaseEncoder<HashedFileWriter> encoder_{writer_};

    /// The file to which the WAL flushes data. The path is fixed, the file gets
    /// moved when the WAL gets rotated.
    std::experimental::filesystem::path current_wal_file_;

    /// Number of deltas in the current wal file.
    int current_wal_file_delta_count_{0};

    /// The latest transaction whose delta is recorded in the current WAL file.
    /// Zero indicates that no deltas have so far been written to the current
    /// WAL file.
    tx::TransactionId latest_tx_{0};

    void RotateFile();
  };

  RingBuffer<database::StateDelta> deltas_;
  utils::Scheduler scheduler_;
  WalFile wal_file_;

  /// Used for disabling the durability feature of the DB.
  bool durability_enabled_{false};
  /// Used for disabling the WAL during DB recovery.
  bool enabled_{false};
  /// Should every WAL write be synced with the underlying storage.
  bool synchronous_commit_{false};

  /// Checks whether the given state delta represents a transaction end,
  /// TRANSACTION_COMMIT and TRANSACTION_ABORT.
  bool IsStateDeltaTransactionEnd(const database::StateDelta &delta);
};
}  // namespace durability