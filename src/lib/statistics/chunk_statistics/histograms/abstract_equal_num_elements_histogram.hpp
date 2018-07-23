#pragma once

#include <memory>

#include "abstract_histogram.hpp"
#include "types.hpp"

namespace opossum {

class Table;

template <typename T>
class AbstractEqualNumElementsHistogram : virtual public AbstractHistogram<T> {
 public:
  using AbstractHistogram<T>::AbstractHistogram;

  HistogramType histogram_type() const override;

 protected:
  size_t _num_buckets() const override;
  BucketID _bucket_for_value(const T value) const override;
  BucketID _lower_bound_for_value(const T value) const override;
  BucketID _upper_bound_for_value(const T value) const override;

  T _bucket_min(const BucketID index) const override;
  T _bucket_max(const BucketID index) const override;
  uint64_t _bucket_count(const BucketID index) const override;
  uint64_t _total_count() const override;

  /**
   * Returns the number of distinct values that are part of this bucket.
   * This number is precise for the state of the table at time of generation.
   */
  uint64_t _bucket_count_distinct(const BucketID index) const override;

 protected:
  std::vector<T> _mins;
  std::vector<T> _maxs;
  std::vector<uint64_t> _counts;
  uint64_t _distinct_count_per_bucket;
  uint64_t _num_buckets_with_extra_value;
};

}  // namespace opossum
