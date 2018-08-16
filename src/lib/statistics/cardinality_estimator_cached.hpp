#pragma once

#include <memory>

#include "abstract_cardinality_estimator.hpp"

namespace opossum {

class BaseCardinalityCache;

enum class CardinalityEstimationCacheMode {
  ReadAndUpdate, ReadOnly
};

class CardinalityEstimatorCached : public AbstractCardinalityEstimator {
 public:
  CardinalityEstimatorCached(const std::shared_ptr<BaseCardinalityCache>& cache,
                             const CardinalityEstimationCacheMode cache_mode,
                             const std::shared_ptr<AbstractCardinalityEstimator>& fallback_estimator);

  std::optional<Cardinality> estimate(const std::vector<std::shared_ptr<AbstractLQPNode>>& relations,
                       const std::vector<std::shared_ptr<const AbstractJoinPlanPredicate>>& predicates) const override;

 private:
  std::shared_ptr<BaseCardinalityCache> _cache;
  CardinalityEstimationCacheMode _cache_mode;
  std::shared_ptr<AbstractCardinalityEstimator> _fallback_estimator;
};

}  // namespace opossum