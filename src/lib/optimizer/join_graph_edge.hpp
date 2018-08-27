#pragma once

#include <memory>
#include <iostream>

#include "boost/functional/hash.hpp"
#include "boost/dynamic_bitset.hpp"

namespace opossum {

class AbstractExpression;

/**
 * A bitset that represents a subsets of the vertices of a JoinGraph.
 *
 * We use a dynamic bitset here, since it has good support for operations commonly performed on join vertex sets, such
 * as union, intersection and subtraction.
 */
using JoinGraphVertexSet = boost::dynamic_bitset<>;

/**
 * Represents a (hyper)edge in a JoinGraph.
 *
 * Each predicate must operate exactly on the vertices in vertex_set. That is, each predicate must reference columns
 * from all vertices in vertex_set and no columns from vertices not in vertex_set. If the predicate wouldn't, then it
 * would belong to another edge.
 */
class JoinGraphEdge final {
 public:
  explicit JoinGraphEdge(const JoinGraphVertexSet& vertex_set, const std::vector<std::shared_ptr<AbstractExpression>>& predicates = {});

  void print(std::ostream& stream = std::cout) const;

  JoinGraphVertexSet vertex_set;
  std::vector<std::shared_ptr<AbstractExpression>> predicates;
};

}  // namespace opossum

namespace std {
template<typename B, typename A>
struct hash<boost::dynamic_bitset<B, A>> {
  std::size_t operator()(const boost::dynamic_bitset<B, A> &v) const {
    for (const auto& block : v)

    return boost::hash_value(v.to_ulong());
  }
};
}