// dw_unit [suite]: every registered test, or one suite's (check.hpp).

#include "check.hpp"

auto main(int argc, char** argv) -> int
{
    return check::run(argc, argv);
}
