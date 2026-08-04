#include "ubsm_test_common.h"

namespace {

enum class Operation {
    Query,
    Remove,
};

struct Options {
    Operation operation = Operation::Query;
    std::string name;
};

void usage(const char *program)
{
    std::cerr << "Usage:\n"
              << "  " << program << " --query NAME\n"
              << "  " << program << " --remove NAME\n";
}

Options parse_options(int argc, char **argv)
{
    if (argc == 2 && (std::string(argv[1]) == "--help" ||
                      std::string(argv[1]) == "-h")) {
        usage(argv[0]);
        std::exit(0);
    }
    if (argc != 3) {
        usage(argv[0]);
        ubsm_test::fail("exactly one operation and one shared-memory name are required");
    }

    Options options;
    const std::string operation(argv[1]);
    if (operation == "--query")
        options.operation = Operation::Query;
    else if (operation == "--remove")
        options.operation = Operation::Remove;
    else {
        usage(argv[0]);
        ubsm_test::fail("unknown operation: " + operation);
    }
    options.name = argv[2];
    ubsm_test::validate_name(options.name);
    return options;
}

int lookup(const std::string &name, ubsmem_shmem_info_t &info)
{
    std::memset(&info, 0, sizeof(info));
    return ubsmem_shmem_lookup(name.c_str(), &info);
}

void print_info(const ubsmem_shmem_info_t &info)
{
    std::cout << "FOUND name=" << info.name << " size=" << info.size
              << " mem_num=" << info.mem_num
              << " mem_unit_size=" << info.mem_unit_size << '\n';
}

} // namespace

int main(int argc, char **argv)
{
    try {
        const Options options = parse_options(argc, argv);

        ubsm_test::check_ubsm(ubsmem_set_logger_level(3),
                              "ubsmem_set_logger_level");
        ubsm_test::SdkSession session;

        ubsmem_shmem_info_t info{};
        const int query_result = lookup(options.name, info);
        if (query_result == UBSM_ERR_NOT_FOUND) {
            std::cout << "NOT_FOUND name=" << options.name << '\n';
            session.finalize();
            return options.operation == Operation::Query ? 1 : 0;
        }
        ubsm_test::check_ubsm(query_result,
                              "ubsmem_shmem_lookup(" + options.name + ")");
        print_info(info);

        if (options.operation == Operation::Query) {
            session.finalize();
            return 0;
        }

        const int remove_result = ubsmem_shmem_deallocate(options.name.c_str());
        if (remove_result == UBSM_ERR_IN_USING) {
            ubsm_test::fail("cannot remove " + options.name +
                            ": it is still mapped or referenced (UBSM_ERR_IN_USING=6024)");
        }
        ubsm_test::check_ubsm(remove_result,
                              "ubsmem_shmem_deallocate(" + options.name + ")");
        std::cout << "REMOVED name=" << options.name << '\n';

        session.finalize();
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
