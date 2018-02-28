#include "dp_ccp_top_k.hpp"

#include <queue>
#include <unordered_map>

#include "abstract_join_plan_node.hpp"
#include "dp_subplan_cache_top_k.hpp"
#include "enumerate_ccp.hpp"
#include "join_edge.hpp"
#include "join_plan_join_node.hpp"
#include "join_plan_vertex_node.hpp"

#define VERBOSE 0

namespace opossum {

DpCcpTopK::DpCcpTopK(const std::shared_ptr<const JoinGraph>& join_graph, const size_t max_entry_count_per_set) : AbstractDpAlgorithm(join_graph, std::make_shared<DpSubplanCacheTopK>(max_entry_count_per_set)) {}

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

    const auto first_vertex_idx = edge->vertex_set.find_first();
    const auto second_vertex_idx = edge->vertex_set.find_next(first_vertex_idx);

    enumerate_ccp_edges.emplace_back(first_vertex_idx, second_vertex_idx);
  }

  /**
   * Actual DpCcpTopK algorithm
   */
  const auto csg_cmp_pairs = EnumerateCcp{_join_graph->vertices.size(), enumerate_ccp_edges}();
  for (const auto& csg_cmp_pair : csg_cmp_pairs) {

    const auto predicates = _join_graph->find_predicates(csg_cmp_pair.first, csg_cmp_pair.second);

    const auto best_plans_left = subplan_cache()->get_best_plans(csg_cmp_pair.first);
    const auto best_plans_right = subplan_cache()->get_best_plans(csg_cmp_pair.second);

#if VERBOSE
    std::cout << "Considering plans " << (csg_cmp_pair.first | csg_cmp_pair.second) << ": " << csg_cmp_pair.first << "(" << best_plans_left.size() << ")"
              << " + " << csg_cmp_pair.second << "(" << best_plans_right.size() << ")" << std::endl;
#endif

    for (const auto& plan_left : best_plans_left) {
      for (const auto& plan_right : best_plans_right) {
        const auto plan_left_right = std::make_shared<JoinPlanJoinNode>(plan_left, plan_right, predicates);
        const auto plan_right_left = std::make_shared<JoinPlanJoinNode>(plan_right, plan_left, predicates);
//        std::cout << "  Caching plan with cost " << plan_left_right->plan_cost() << std::endl;
//        std::cout << "  - Caching plan with cost " << plan_right_left->plan_cost() << std::endl;
        subplan_cache()->cache_plan(csg_cmp_pair.first | csg_cmp_pair.second, plan_left_right);
        subplan_cache()->cache_plan(csg_cmp_pair.first | csg_cmp_pair.second, plan_right_left);
      }
    }
  }
}

}  // namespace opossum