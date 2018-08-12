#include "dp_ccp_top_k.hpp"

#include <queue>
#include <unordered_map>

#include "abstract_join_plan_node.hpp"
#include "build_join_plan.hpp"
#include "dp_subplan_cache_top_k.hpp"
#include "enumerate_ccp.hpp"
#include "join_edge.hpp"
#include "join_plan_join_node.hpp"
#include "join_plan_vertex_node.hpp"
#include "lqp_blacklist.hpp"

#define VERBOSE 0

namespace opossum {

DpCcpTopK::DpCcpTopK(const size_t max_entry_count_per_set, const std::shared_ptr<const AbstractCostModel>& cost_model,
                     const std::shared_ptr<LQPBlacklist>& lqp_blacklist,
                     const std::shared_ptr<AbstractCardinalityEstimator>& cardinality_estimator)
    : AbstractDpAlgorithm(std::make_shared<DpSubplanCacheTopK>(max_entry_count_per_set), cost_model, cardinality_estimator, lqp_blacklist) {}

std::shared_ptr<DpSubplanCacheTopK> DpCcpTopK::subplan_cache() {
  return std::static_pointer_cast<DpSubplanCacheTopK>(_subplan_cache);
}

std::shared_ptr<const DpSubplanCacheTopK> DpCcpTopK::subplan_cache() const {
  return std::static_pointer_cast<DpSubplanCacheTopK>(_subplan_cache);
}

void DpCcpTopK::_on_execute() {
  /**
   * Build `enumerate_ccp_edges` from all vertex-to-vertex edges for EnumerateCcp,
   */
  std::vector<std::pair<size_t, size_t>> enumerate_ccp_edges;
  for (const auto& edge : _join_graph->edges) {
    if (edge->vertex_set.count() != 2) continue;
    Assert(edge->vertex_set.count() == 2, "Can't place complex predicates yet");

    const auto first_vertex_idx = edge->vertex_set.find_first();
    const auto second_vertex_idx = edge->vertex_set.find_next(first_vertex_idx);

    enumerate_ccp_edges.emplace_back(first_vertex_idx, second_vertex_idx);
  }


#if VERBOSE
  for (auto vertex_idx = size_t{0}; vertex_idx < _join_graph->vertices.size(); ++vertex_idx) {
    std::cout << vertex_idx << ": " << _join_graph->vertices[vertex_idx]->description() << std::endl;
  }
#endif

  /**
   * Actual DpCcpTopK algorithm
   */
  const auto csg_cmp_pairs = EnumerateCcp{_join_graph->vertices.size(), enumerate_ccp_edges}();
  for (const auto& csg_cmp_pair : csg_cmp_pairs) {
    const auto predicates = _join_graph->find_predicates(csg_cmp_pair.first, csg_cmp_pair.second);

    const auto best_plans_left = subplan_cache()->get_best_plans(csg_cmp_pair.first);
    const auto best_plans_right = subplan_cache()->get_best_plans(csg_cmp_pair.second);

#if VERBOSE
    std::cout << "Considering plans " << (csg_cmp_pair.first | csg_cmp_pair.second) << ": " << csg_cmp_pair.first << "("
              << best_plans_left.size() << ")"
              << " + " << csg_cmp_pair.second << "(" << best_plans_right.size() << ")" << std::endl;
#endif

    for (const auto& plan_left : best_plans_left) {
      for (const auto& plan_right : best_plans_right) {
        auto current_plan = build_join_plan_join_node(*_cost_model, plan_left, plan_right, predicates, *_cardinality_estimator);
        if (_lqp_blacklist && _lqp_blacklist->test(current_plan.lqp)) {
          current_plan.plan_cost = std::numeric_limits<Cost>::infinity();
        }

        subplan_cache()->cache_plan(csg_cmp_pair.first | csg_cmp_pair.second, current_plan);
      }
    }
  }
}

}  // namespace opossum