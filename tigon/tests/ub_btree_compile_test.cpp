#include "core/UBBPlusTree.h"

#include <cstdint>
#include <type_traits>

namespace
{

struct TestKey {
        uint64_t value;
};

struct TestValue {
        uint64_t value;
};

struct TestComparator {
        int operator()(const TestKey &a, const TestKey &b) const
        {
                return a.value < b.value ? -1 : (a.value > b.value ? 1 : 0);
        }
};

using TestTree = star::UBBPlusTree<TestKey, TestValue, TestComparator, 3, 3>;

static_assert(std::is_trivially_copyable<TestTree::LeafEntry>::value,
              "leaf entries are copied during split");
static_assert(alignof(TestTree::LeafNode) == 64,
              "leaf nodes must keep their shared ABI alignment");
static_assert(alignof(TestTree::TupleNode) == 64,
              "stable tuples must keep their lock alignment");

} // namespace

template class star::UBBPlusTree<TestKey, TestValue, TestComparator, 3, 3>;

int main()
{
        return 0;
}
