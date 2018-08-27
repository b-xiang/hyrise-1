#pragma once

#include <tbb/concurrent_vector.h>
#include <boost/container/pmr/memory_resource.hpp>

// the linter wants this to be above everything else
#include <shared_mutex>

#include <algorithm>
#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "index/column_index_type.hpp"

#include "all_type_variant.hpp"
#include "chunk_access_counter.hpp"
#include "mvcc_data.hpp"
#include "table_cxlumn_definition.hpp"
#include "types.hpp"
#include "utils/copyable_atomic.hpp"
#include "utils/scoped_locking_ptr.hpp"

namespace opossum {

class BaseIndex;
class BaseSegment;
class ChunkStatistics;

using ChunkSegments = pmr_vector<std::shared_ptr<BaseSegment>>;

/**
 * A Chunk is a horizontal partition of a table.
 * It stores the table's data column by column.
 * Optionally, mostly applying to StoredTables, it may also hold MvccData.
 *
 * Find more information about this in our wiki: https://github.com/hyrise/hyrise/wiki/chunk-concept
 */
class Chunk : private Noncopyable {
 public:
  // The last chunk offset is reserved for NULL as used in ReferenceSegments.
  static constexpr ChunkOffset MAX_SIZE = std::numeric_limits<ChunkOffset>::max() - 1;

  Chunk(const ChunkSegments& columns, const std::shared_ptr<MvccData>& mvcc_data = nullptr,
        const std::optional<PolymorphicAllocator<Chunk>>& alloc = std::nullopt,
        const std::shared_ptr<ChunkAccessCounter>& access_counter = nullptr);

  // returns whether new rows can be appended to this Chunk
  bool is_mutable() const;

  void mark_immutable();

  // Atomically replaces the current column at cxlumn_id with the passed column
  void replace_column(size_t cxlumn_id, const std::shared_ptr<BaseSegment>& column);

  // returns the number of columns (cannot exceed CxlumnID (uint16_t))
  uint16_t cxlumn_count() const;

  // returns the number of rows (cannot exceed ChunkOffset (uint32_t))
  uint32_t size() const;

  // adds a new row, given as a list of values, to the chunk
  // note this is slow and not thread-safe and should be used for testing purposes only
  void append(const std::vector<AllTypeVariant>& values);

  /**
   * Atomically accesses and returns the column at a given position
   *
   * Note: Concurrently with the execution of operators,
   *       ValueSegments might be exchanged with DictionarySegments.
   *       Therefore, if you hold a pointer to a column, you can
   *       continue to use it without any inconsistencies.
   *       However, if you call get_segment again, be aware that
   *       the return type might have changed.
   */
  std::shared_ptr<BaseSegment> get_segment(CxlumnID cxlumn_id) const;

  const ChunkSegments& columns() const;

  bool has_mvcc_data() const;
  bool has_access_counter() const;

  /**
   * The locking pointer locks the columns non-exclusively
   * and unlocks them on destruction
   *
   * For improved performance, it is best to call this function
   * once and retain the reference as long as needed.
   *
   * @return a locking ptr to the mvcc columns
   */
  SharedScopedLockingPtr<MvccData> get_scoped_mvcc_data_lock();
  SharedScopedLockingPtr<const MvccData> get_scoped_mvcc_data_lock() const;

  std::shared_ptr<MvccData> mvcc_data() const;
  void set_mvcc_data(const std::shared_ptr<MvccData>& mvcc_data);

  std::vector<std::shared_ptr<BaseIndex>> get_indices(
      const std::vector<std::shared_ptr<const BaseSegment>>& columns) const;
  std::vector<std::shared_ptr<BaseIndex>> get_indices(const std::vector<CxlumnID>& cxlumn_ids) const;

  std::shared_ptr<BaseIndex> get_index(const SegmentIndexType index_type,
                                       const std::vector<std::shared_ptr<const BaseSegment>>& columns) const;
  std::shared_ptr<BaseIndex> get_index(const SegmentIndexType index_type, const std::vector<CxlumnID>& cxlumn_ids) const;

  template <typename Index>
  std::shared_ptr<BaseIndex> create_index(const std::vector<std::shared_ptr<const BaseSegment>>& index_columns) {
    DebugAssert(([&]() {
                  for (auto column : index_columns) {
                    const auto column_it = std::find(_columns.cbegin(), _columns.cend(), column);
                    if (column_it == _columns.cend()) return false;
                  }
                  return true;
                }()),
                "All columns must be part of the chunk.");

    auto index = std::make_shared<Index>(index_columns);
    _indices.emplace_back(index);
    return index;
  }

  template <typename Index>
  std::shared_ptr<BaseIndex> create_index(const std::vector<CxlumnID>& cxlumn_ids) {
    const auto columns = _get_segments_for_ids(cxlumn_ids);
    return create_index<Index>(columns);
  }

  void remove_index(const std::shared_ptr<BaseIndex>& index);

  void migrate(boost::container::pmr::memory_resource* memory_source);

  std::shared_ptr<ChunkAccessCounter> access_counter() const { return _access_counter; }

  bool references_exactly_one_table() const;

  const PolymorphicAllocator<Chunk>& get_allocator() const;

  std::shared_ptr<ChunkStatistics> statistics() const;

  void set_statistics(const std::shared_ptr<ChunkStatistics>& chunk_statistics);

  /**
   * For debugging purposes, makes an estimation about the memory used by this Chunk and its Columns
   */
  size_t estimate_memory_usage() const;

 private:
  std::vector<std::shared_ptr<const BaseSegment>> _get_segments_for_ids(const std::vector<CxlumnID>& cxlumn_ids) const;

 private:
  PolymorphicAllocator<Chunk> _alloc;
  ChunkSegments _columns;
  std::shared_ptr<MvccData> _mvcc_data;
  std::shared_ptr<ChunkAccessCounter> _access_counter;
  pmr_vector<std::shared_ptr<BaseIndex>> _indices;
  std::shared_ptr<ChunkStatistics> _statistics;
  bool _is_mutable = true;
};

}  // namespace opossum
