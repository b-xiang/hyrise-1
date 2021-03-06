#pragma once

#include <algorithm>
#include <limits>
#include <memory>

#include "storage/base_column_encoder.hpp"

#include "storage/dictionary_column.hpp"
#include "storage/fixed_string_dictionary_column.hpp"
#include "storage/value_column.hpp"
#include "storage/vector_compression/base_compressed_vector.hpp"

#include "storage/vector_compression/vector_compression.hpp"
#include "types.hpp"
#include "utils/enum_constant.hpp"

namespace opossum {

/**
 * @brief Encodes a column using dictionary encoding and compresses its attribute vector using vector compression.
 *
 * The algorithm first creates an attribute vector of standard size (uint32_t) and then compresses it
 * using fixed-size byte-aligned encoding.
 */
template <auto Encoding>
class DictionaryEncoder : public ColumnEncoder<DictionaryEncoder<Encoding>> {
 public:
  static constexpr auto _encoding_type = enum_c<EncodingType, Encoding>;
  static constexpr auto _uses_vector_compression = true;  // see base_column_encoder.hpp for details

  template <typename T>
  std::shared_ptr<BaseEncodedColumn> _on_encode(const std::shared_ptr<const ValueColumn<T>>& value_column) {
    // See: https://goo.gl/MCM5rr
    // Create dictionary (enforce uniqueness and sorting)
    const auto& values = value_column->values();

    if constexpr (Encoding == EncodingType::FixedStringDictionary) {
      // Encode a column with a FixedStringVector as dictionary. std::string is the only supported type
      return _encode_dictionary_column(
          FixedStringVector{values.cbegin(), values.cend(), _calculate_fixed_string_length(values), values.size()},
          value_column);
    } else {
      // Encode a column with a pmr_vector<T> as dictionary
      return _encode_dictionary_column(pmr_vector<T>{values.cbegin(), values.cend(), values.get_allocator()},
                                       value_column);
    }
  }

 private:
  template <typename U, typename T>
  static ValueID _get_value_id(const U& dictionary, const T& value) {
    return static_cast<ValueID>(
        std::distance(dictionary.cbegin(), std::lower_bound(dictionary.cbegin(), dictionary.cend(), value)));
  }

  template <typename U, typename T>
  std::shared_ptr<BaseEncodedColumn> _encode_dictionary_column(
      U dictionary, const std::shared_ptr<const ValueColumn<T>>& value_column) {
    const auto& values = value_column->values();
    const auto alloc = values.get_allocator();

    // Remove null values from value vector
    if (value_column->is_nullable()) {
      const auto& null_values = value_column->null_values();

      // Swap values to back if value is null
      auto erase_from_here_it = dictionary.end();
      auto null_it = null_values.crbegin();
      for (auto dict_it = dictionary.rbegin(); dict_it != dictionary.rend(); ++dict_it, ++null_it) {
        if (*null_it) {
          std::iter_swap(dict_it, --erase_from_here_it);
        }
      }

      // Erase null values
      dictionary.erase(erase_from_here_it, dictionary.end());
    }

    std::sort(dictionary.begin(), dictionary.end());
    dictionary.erase(std::unique(dictionary.begin(), dictionary.end()), dictionary.end());
    dictionary.shrink_to_fit();

    auto attribute_vector = pmr_vector<uint32_t>{values.get_allocator()};
    attribute_vector.reserve(values.size());

    const auto null_value_id = static_cast<uint32_t>(dictionary.size());

    if (value_column->is_nullable()) {
      const auto& null_values = value_column->null_values();

      /**
       * Iterators are used because values and null_values are of
       * type tbb::concurrent_vector and thus index-based access isn’t in O(1)
       */
      auto value_it = values.cbegin();
      auto null_value_it = null_values.cbegin();
      for (; value_it != values.cend(); ++value_it, ++null_value_it) {
        if (*null_value_it) {
          attribute_vector.push_back(null_value_id);
          continue;
        }

        const auto value_id = _get_value_id(dictionary, *value_it);
        attribute_vector.push_back(value_id);
      }
    } else {
      auto value_it = values.cbegin();
      for (; value_it != values.cend(); ++value_it) {
        const auto value_id = _get_value_id(dictionary, *value_it);
        attribute_vector.push_back(value_id);
      }
    }

    // We need to increment the dictionary size here because of possible null values.
    const auto max_value = dictionary.size() + 1u;

    auto encoded_attribute_vector = compress_vector(
        attribute_vector, ColumnEncoder<DictionaryEncoder<Encoding>>::vector_compression_type(), alloc, {max_value});
    auto dictionary_sptr = std::allocate_shared<U>(alloc, std::move(dictionary));
    auto attribute_vector_sptr = std::shared_ptr<const BaseCompressedVector>(std::move(encoded_attribute_vector));

    if constexpr (Encoding == EncodingType::FixedStringDictionary) {
      return std::allocate_shared<FixedStringDictionaryColumn<T>>(alloc, dictionary_sptr, attribute_vector_sptr,
                                                                  ValueID{null_value_id});
    } else {
      return std::allocate_shared<DictionaryColumn<T>>(alloc, dictionary_sptr, attribute_vector_sptr,
                                                       ValueID{null_value_id});
    }
  }

  size_t _calculate_fixed_string_length(const pmr_concurrent_vector<std::string>& values) const {
    size_t max_string_length = 0;
    for (const auto& value : values) {
      if (value.size() > max_string_length) max_string_length = value.size();
    }
    return max_string_length;
  }
};

}  // namespace opossum
