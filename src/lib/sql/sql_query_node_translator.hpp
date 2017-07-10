#pragma once

#include <memory>
#include <string>
#include <vector>

#include "SQLParser.h"

#include "all_type_variant.hpp"
#include "optimizer/abstract_syntax_tree/abstract_ast_node.hpp"
#include "optimizer/abstract_syntax_tree/expression_node.hpp"

namespace opossum {

class SQLQueryNodeTranslator {
 public:
  SQLQueryNodeTranslator();
  virtual ~SQLQueryNodeTranslator();

  // Translates the given SQL result.
  std::vector<std::shared_ptr<AbstractAstNode>> translate_parse_result(const hsql::SQLParserResult& result);

  std::shared_ptr<AbstractAstNode> translate_statement(const hsql::SQLStatement& statement);

 protected:
  std::shared_ptr<AbstractAstNode> _translate_select(const hsql::SelectStatement& select);

  std::shared_ptr<AbstractAstNode> _translate_table_ref(const hsql::TableRef& table);

  std::shared_ptr<AbstractAstNode> _translate_filter_expr(const hsql::Expr& expr,
                                                       const std::shared_ptr<AbstractAstNode>& input_node);

  std::shared_ptr<AbstractAstNode> _translate_aggregate(const hsql::SelectStatement & select,
                                                        const std::shared_ptr<AbstractAstNode>& input_node);

  std::shared_ptr<AbstractAstNode> _translate_projection(const std::vector<hsql::Expr*>& expr_list,
                                                      const std::shared_ptr<AbstractAstNode>& input_node);

  std::shared_ptr<AbstractAstNode> _translate_order_by(const std::vector<hsql::OrderDescription*> order_list,
                                                    const std::shared_ptr<AbstractAstNode>& input_node);

  std::shared_ptr<ExpressionNode> _translate_expression(const hsql::Expr& expr);

  //  std::shared_ptr<AbstractAstNode> _translate_join(const hsql::JoinDefinition& select);

  const std::string _get_column_name(const hsql::Expr& expr) const;

  const AllTypeVariant _translate_literal(const hsql::Expr& expr);
};

}  // namespace opossum
