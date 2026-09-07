#include "benchmark/ycsb/Schema.h"
#include "core/UBBPlusTreeAdapter.h"

#include <type_traits>

using YCSBUBTable =
        star::TableUBBPlusTree<star::ycsb::ycsb::key, star::ycsb::ycsb::value,
                               star::ycsb::ycsb::KeyComparator,
                               star::ycsb::ycsb::ValueComparator>;

static_assert(!std::is_abstract<YCSBUBTable>::value,
              "TableUBBPlusTree must implement the complete ITable interface");

int main()
{
        return 0;
}
