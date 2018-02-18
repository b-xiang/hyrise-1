#pragma once

#include <boost/preprocessor/seq/for_each_product.hpp>
#include <boost/preprocessor/tuple/elem.hpp>

#include "jit_types.hpp"
#include "operators/table_scan/like_table_scan_impl.hpp"

namespace opossum {

/* This file contains the type dispatching mechanisms that allow generic operations on JitMaterializedValues.
 *
 * Each binary operation requires three JitMaterializedValues - a left input (lhs), a right input (rhs) and an output (result).
 * Each value has one of 6 data types (bool, int, long, float, double, std::string) and can be nullable or non-nullable. This leaves us with
 * (6 * 2) * (6 * 2) = 144 combinations for each operation.
 *
 * To make things easier, all arithmetic and comparison operations can be handled the same way:
 * A set of generic lambdas defines type-independent versions of these operations. These lambdas can be passed to the "jit_compute" function
 * to perform the actual computation. The lambdas work on raw, concrete values. It is the responsibility of "jit_compute"
 * to take care of NULL values, unpack input values and pack the result value. This way all NULL-value semantics are kept in one place.
 * If either of the inputs is NULL, the result of the computation is also NULL. If neither input is NULL, the computation lambda is called.
 *
 * Inside "jit_compute", a switch statement (generated by the preprocessor) dispatches the 36 data-type combinations and calls the
 * lambda with the appropriately typed parameters. Invalid type combinations (e.g. adding an int32_t and a std::string) are handled
 * via the SFINAE pattern (substitution failure is not an error): The lambdas fail the template substitution if they cannot perform their
 * operation on a given combination of input types. In this case, the "InvalidTypeCatcher" provides a default implementation that
 * throws an exception.
 *
 * The generic lambdas can also be passed to the "jit_compute_type" function. The function uses the same dispatching mechanisms.
 * But instead of executing a computation, it only determines the result type the computation would have if it were carried out.
 * This functionality is used to determine the type of intermediate values and computed output columns.
 *
 * Logical operators, IsNull and IsNotNull must be handled separately, since their NULL value semantics are different (i.e. a NULL as
 * either input does not result in the output being NULL as well).
 */

#define JIT_APPEND_ENUM_NAMESPACE(enum_value) JitDataType::enum_value
#define JIT_GET_ENUM_VALUE(index, s) JIT_APPEND_ENUM_NAMESPACE(BOOST_PP_TUPLE_ELEM(3, 1, BOOST_PP_SEQ_ELEM(index, s)))
#define JIT_GET_DATA_TYPE(index, s) BOOST_PP_TUPLE_ELEM(3, 0, BOOST_PP_SEQ_ELEM(index, s))

#define JIT_COMPUTE_CASE(r, types)                                                                                   \
  case static_cast<uint8_t>(JIT_GET_ENUM_VALUE(0, types)) << 8 | static_cast<uint8_t>(JIT_GET_ENUM_VALUE(1, types)): \
    catching_func(lhs.as<JIT_GET_DATA_TYPE(0, types)>(), rhs.as<JIT_GET_DATA_TYPE(1, types)>(), result);             \
    break;

#define JIT_COMPUTE_TYPE_CASE(r, types)                                                                              \
  case static_cast<uint8_t>(JIT_GET_ENUM_VALUE(0, types)) << 8 | static_cast<uint8_t>(JIT_GET_ENUM_VALUE(1, types)): \
    return catching_func(JIT_GET_DATA_TYPE(0, types)(), JIT_GET_DATA_TYPE(1, types)());

/* Arithmetic operators */
const auto jit_addition = [](const auto& a, const auto& b) -> decltype(a + b) { return a + b; };
const auto jit_subtraction = [](const auto& a, const auto& b) -> decltype(a - b) { return a - b; };
const auto jit_multiplication = [](const auto& a, const auto& b) -> decltype(a * b) { return a * b; };
const auto jit_division = [](const auto& a, const auto& b) -> decltype(a / b) { return a / b; };
const auto jit_modulo = [](const auto& a, const auto& b) -> decltype(a % b) { return a % b; };
const auto jit_power = [](const auto& a, const auto& b) -> decltype(std::pow(a, b)) { return std::pow(a, b); };

/* Comparison operators */
const auto jit_equals = [](const auto& a, const auto& b) -> decltype(a == b, uint8_t()) { return a == b; };
const auto jit_not_equals = [](const auto& a, const auto& b) -> decltype(a != b, uint8_t()) { return a != b; };
const auto jit_less_than = [](const auto& a, const auto& b) -> decltype(a < b, uint8_t()) { return a < b; };
const auto jit_less_than_equals = [](const auto& a, const auto& b) -> decltype(a <= b, uint8_t()) { return a <= b; };
const auto jit_greater_than = [](const auto& a, const auto& b) -> decltype(a > b, uint8_t()) { return a > b; };
const auto jit_greater_than_equals = [](const auto& a, const auto& b) -> decltype(a >= b, uint8_t()) { return a >= b; };

const auto jit_like = [](const std::string& a, const std::string& b) -> uint8_t {
  const auto regex_string = LikeTableScanImpl::sqllike_to_regex(b);
  const auto regex = std::regex{regex_string, std::regex_constants::icase};
  return std::regex_match(a, regex);
};

const auto jit_not_like = [](const std::string& a, const std::string& b) -> uint8_t {
  const auto regex_string = LikeTableScanImpl::sqllike_to_regex(b);
  const auto regex = std::regex{regex_string, std::regex_constants::icase};
  return !std::regex_match(a, regex);
};

// The InvalidTypeCatcher acts as a fallback inplementation, if template specialization
// fails for a type combination.
template <typename F, typename R>
struct InvalidTypeCatcher : F {
  explicit InvalidTypeCatcher(F f) : F(std::move(f)) {}

  using F::operator();

  template <typename... Ts>
  R operator()(const Ts...) const {
    Fail("invalid combination of types for operation");
  }
};

template <typename T>
__attribute__((noinline)) void jit_compute(const T& op_func, const JitMaterializedValue& lhs,
                                           const JitMaterializedValue& rhs, JitMaterializedValue& result) {
  // Handle NULL values and return if either input is NULL.
  result.is_null() = lhs.is_null() || rhs.is_null();
  if (result.is_null()) {
    return;
  }

  const auto func = [&](const auto& typed_lhs, const auto& typed_rhs, auto& result) -> decltype(
      op_func(typed_lhs, typed_rhs), void()) {
    using ResultType = decltype(op_func(typed_lhs, typed_rhs));
    result.template as<ResultType>() = op_func(typed_lhs, typed_rhs);
  };

  const auto catching_func = InvalidTypeCatcher<decltype(func), void>(func);

  // The type information from the lhs and rhs are combined into a single value for dispatching without nesting.
  const auto combined_types =
      static_cast<uint8_t>(lhs.data_type()) << 8 | static_cast<uint8_t>(rhs.data_type()) switch (combined_types) {
    BOOST_PP_SEQ_FOR_EACH_PRODUCT(JIT_COMPUTE_CASE, (JIT_DATA_TYPE_INFO)(JIT_DATA_TYPE_INFO))
    default:
      Fail("unreachable");
  }
}

template <typename T>
__attribute__((noinline)) JitDataType jit_compute_type(const T& op_func, const JitDataType lhs, const JitDataType rhs) {
  const auto func = [&](const auto& typed_lhs, const auto& typed_rhs) -> decltype(op_func(typed_lhs, typed_rhs),
                                                                                  JitDataType()) {
    using ResultType = decltype(op_func(typed_lhs, typed_rhs));
    // This templated function returns the JitDataType enum value for a given ResultType.
    return jit_data_type<ResultType>();
  };

  const auto catching_func = InvalidTypeCatcher<decltype(func), JitDataType>(func);

  // The type information from the lhs and rhs are combined into a single value for dispatching without nesting.
  switch (static_cast<uint8_t>(lhs) << 8 | static_cast<uint8_t>(rhs)) {
    BOOST_PP_SEQ_FOR_EACH_PRODUCT(JIT_COMPUTE_TYPE_CASE, (JIT_DATA_TYPE_INFO)(JIT_DATA_TYPE_INFO))
    default:
      Fail("unreachable");
  }
}

__attribute__((noinline)) void jit_not(const JitMaterializedValue& lhs, JitMaterializedValue& result) {
  DebugAssert(lhs.data_type() == JitDataType::Bool && result.data_type() == JitDataType::Bool,
              "invalid type for operation");
  result.is_null() = lhs.is_null();
  result.as<uint8_t>() = !lhs.as<uint8_t>();
}

__attribute__((noinline)) void jit_and(const JitMaterializedValue& lhs, const JitMaterializedValue& rhs,
                                       JitMaterializedValue& result) {
  DebugAssert(lhs.data_type() == JitDataType::Bool && rhs.data_type() == JitDataType::Bool &&
                  result.data_type() == JitDataType::Bool,
              "invalid type for operation");

  // Thee-valued logic AND
  if (lhs.is_null()) {
    result.as<uint8_t>() = false;
    result.is_null() = rhs.is_null() || rhs.as<uint8_t>();
  } else {
    result.as<uint8_t>() = lhs.as<uint8_t>() && rhs.as<uint8_t>();
    result.is_null() = lhs.as<uint8_t>() && rhs.is_null();
  }
}

__attribute__((noinline)) void jit_or(const JitMaterializedValue& lhs, const JitMaterializedValue& rhs,
                                      JitMaterializedValue& result) {
  DebugAssert(lhs.data_type() == JitDataType::Bool && rhs.data_type() == JitDataType::Bool &&
                  result.data_type() == JitDataType::Bool,
              "invalid type for operation");

  // Thee-valued logic OR
  if (lhs.is_null()) {
    result.as<uint8_t>() = true;
    result.is_null() = rhs.is_null() || !rhs.as<uint8_t>();
  } else {
    result.as<uint8_t>() = lhs.as<uint8_t>() || rhs.as<uint8_t>();
    result.is_null() = !lhs.as<uint8_t>() && rhs.is_null();
  }
}

__attribute__((noinline)) void jit_is_null(const JitMaterializedValue& lhs, JitMaterializedValue& result) {
  DebugAssert(result.data_type() == JitDataType::Bool, "invalid type for operation");

  result.is_null() = false;
  result.as<uint8_t>() = lhs.is_null();
}

__attribute__((noinline)) void jit_is_not_null(const JitMaterializedValue& lhs, JitMaterializedValue& result) {
  DebugAssert(result.data_type() == JitDataType::Bool, "invalid type for operation");

  result.is_null() = false;
  result.as<uint8_t>() = !lhs.is_null();
}

}  // namespace opossum
