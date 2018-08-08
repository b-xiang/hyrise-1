#include "equal_num_elements_histogram.hpp"

#include <memory>
#include <numeric>

#include "storage/table.hpp"
#include "storage/value_column.hpp"

namespace opossum {

template <typename T>
HistogramType EqualNumElementsHistogram<T>::histogram_type() const {
  return HistogramType::EqualNumElements;
}

template <typename T>
size_t EqualNumElementsHistogram<T>::num_buckets() const {
  return _counts.size();
}

template <typename T>
BucketID EqualNumElementsHistogram<T>::_bucket_for_value(const T value) const {
  const auto it = std::lower_bound(_maxs.begin(), _maxs.end(), value);
  const auto index = static_cast<BucketID>(std::distance(_maxs.begin(), it));

  if (it == _maxs.end() || value < _bucket_min(index) || value > _bucket_max(index)) {
    return INVALID_BUCKET_ID;
  }

  return index;
}

template <typename T>
BucketID EqualNumElementsHistogram<T>::_lower_bound_for_value(const T value) const {
  const auto it = std::lower_bound(_maxs.begin(), _maxs.end(), value);
  const auto index = static_cast<BucketID>(std::distance(_maxs.begin(), it));

  if (it == _maxs.end()) {
    return INVALID_BUCKET_ID;
  }

  return index;
}

template <typename T>
BucketID EqualNumElementsHistogram<T>::_upper_bound_for_value(const T value) const {
  const auto it = std::upper_bound(_maxs.begin(), _maxs.end(), value);
  const auto index = static_cast<BucketID>(std::distance(_maxs.begin(), it));

  if (it == _maxs.end()) {
    return INVALID_BUCKET_ID;
  }

  return index;
}

template <typename T>
T EqualNumElementsHistogram<T>::_bucket_min(const BucketID index) const {
  DebugAssert(index < _mins.size(), "Index is not a valid bucket.");
  return _mins[index];
}

template <typename T>
T EqualNumElementsHistogram<T>::_bucket_max(const BucketID index) const {
  DebugAssert(index < _maxs.size(), "Index is not a valid bucket.");
  return _maxs[index];
}

template <typename T>
uint64_t EqualNumElementsHistogram<T>::_bucket_count(const BucketID index) const {
  DebugAssert(index < _counts.size(), "Index is not a valid bucket.");
  return _counts[index];
}

template <typename T>
uint64_t EqualNumElementsHistogram<T>::_bucket_count_distinct(const BucketID index) const {
  return _distinct_count_per_bucket + (index < _num_buckets_with_extra_value ? 1 : 0);
}

template <typename T>
uint64_t EqualNumElementsHistogram<T>::total_count() const {
  return std::accumulate(_counts.begin(), _counts.end(), 0ul);
}

template <typename T>
uint64_t EqualNumElementsHistogram<T>::total_count_distinct() const {
  return _distinct_count_per_bucket * num_buckets() + _num_buckets_with_extra_value;
}

template <typename T>
void EqualNumElementsHistogram<T>::_generate(const std::shared_ptr<const ValueColumn<T>> distinct_column,
                                             const std::shared_ptr<const ValueColumn<int64_t>> count_column,
                                             const size_t max_num_buckets) {
  // If there are fewer distinct values than the number of desired buckets use that instead.
  const auto distinct_count = distinct_column->size();
  const auto num_buckets = distinct_count < max_num_buckets ? static_cast<size_t>(distinct_count) : max_num_buckets;

  // Split values evenly among buckets.
  _distinct_count_per_bucket = distinct_count / num_buckets;
  _num_buckets_with_extra_value = distinct_count % num_buckets;

  auto begin_index = 0ul;
  for (BucketID bucket_index = 0; bucket_index < num_buckets; bucket_index++) {
    auto end_index = begin_index + _distinct_count_per_bucket - 1;
    if (bucket_index < _num_buckets_with_extra_value) {
      end_index++;
    }

    const auto current_min = *(distinct_column->values().cbegin() + begin_index);
    const auto current_max = *(distinct_column->values().cbegin() + end_index);

    if constexpr (std::is_same_v<T, std::string>) {
      Assert(current_min.find_first_not_of(this->_supported_characters) == std::string::npos,
             "Unsupported characters.");
      Assert(current_max.find_first_not_of(this->_supported_characters) == std::string::npos,
             "Unsupported characters.");
    }

    _mins.emplace_back(current_min);
    _maxs.emplace_back(current_max);
    _counts.emplace_back(std::accumulate(count_column->values().cbegin() + begin_index,
                                         count_column->values().cbegin() + end_index + 1, uint64_t{0}));

    begin_index = end_index + 1;
  }
}

EXPLICITLY_INSTANTIATE_DATA_TYPES(EqualNumElementsHistogram);

}  // namespace opossum