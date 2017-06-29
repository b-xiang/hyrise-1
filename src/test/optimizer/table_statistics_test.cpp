#include <memory>
#include <string>
#include <vector>

#include "../base_test.hpp"
#include "gtest/gtest.h"

#include "optimizer/table_statistics.hpp"
#include "storage/storage_manager.hpp"

namespace opossum {

    class TableStatisticsTest : public BaseTest {
    protected:
        void SetUp() override {
          std::shared_ptr<Table> table_a = load_table("src/test/tables/int_float.tbl", 0);
          StorageManager::get().add_table("table_a", std::move(table_a));

          table_a_stats = std::make_shared<TableStatistics>("table_a");

//          std::shared_ptr<Table> table_b = load_table("src/test/tables/int_float2.tbl", 0);
//          StorageManager::get().add_table("table_b", std::move(table_b));
//
//          table_b_stats = std::make_shared<TableStatistics>("table_b");

//          std::shared_ptr<Table> table_c = load_table("src/test/tables/int_string.tbl", 4);
//          StorageManager::get().add_table("table_c", std::move(table_c));
//
//          std::shared_ptr<Table> table_d = load_table("src/test/tables/string_int.tbl", 3);
//          StorageManager::get().add_table("table_d", std::move(table_d));
//
//          std::shared_ptr<Table> test_table2 = load_table("src/test/tables/int_string2.tbl", 2);
//          StorageManager::get().add_table("TestTable", test_table2);
        }

        std::shared_ptr<TableStatistics> table_a_stats;
        std::shared_ptr<TableStatistics> table_b_stats;
    };

    TEST_F(TableStatisticsTest, SimpleTest) {
      ASSERT_EQ(table_a_stats->row_count(), 3.);
      auto stat1 = table_a_stats->predicate_statistics("a", "!=", opossum::AllParameterVariant(123));
      ASSERT_EQ(stat1->row_count(), 2.);
//      auto stat2 = stat1->predicate_statistics("C_D_ID", "!=", opossum::AllParameterVariant(2));
      auto stat2 = stat1->predicate_statistics("b", "<", opossum::AllParameterVariant(458.2f));
      ASSERT_GT(stat2->row_count(), 1.);
      ASSERT_LT(stat2->row_count(), 2.);
    }

}  // namespace opossum
