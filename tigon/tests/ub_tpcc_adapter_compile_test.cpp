#include "benchmark/tpcc/Schema.h"
#include "core/UBBPlusTreeAdapter.h"

#include <type_traits>

namespace tpcc = star::tpcc;

#define CHECK_UB_TPCC_TABLE(schema)                                            \
        using schema##_ub_table = star::TableUBBPlusTree<                     \
                tpcc::schema::key, tpcc::schema::value,                       \
                tpcc::schema::KeyComparator, tpcc::schema::ValueComparator>;  \
        static_assert(!std::is_abstract<schema##_ub_table>::value,             \
                      #schema " UB table must implement ITable")

CHECK_UB_TPCC_TABLE(warehouse);
CHECK_UB_TPCC_TABLE(district);
CHECK_UB_TPCC_TABLE(customer);
CHECK_UB_TPCC_TABLE(customer_name_idx);
CHECK_UB_TPCC_TABLE(history);
CHECK_UB_TPCC_TABLE(new_order);
CHECK_UB_TPCC_TABLE(order);
CHECK_UB_TPCC_TABLE(order_customer);
CHECK_UB_TPCC_TABLE(order_line);
CHECK_UB_TPCC_TABLE(stock);
CHECK_UB_TPCC_TABLE(item);

#undef CHECK_UB_TPCC_TABLE

int main()
{
        return 0;
}
