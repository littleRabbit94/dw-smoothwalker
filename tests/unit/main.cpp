// dw_unit [suite]: every registered test, or one suite's (check.hpp). dw_unit --record-session: rewrites the session
// suite's goldens (session_test.cpp says when).

#include "check.hpp"

#include <cstring>

auto record_session() -> int; // session_test.cpp

auto main(int argc, char** argv) -> int
{
    if (argc > 1 && std::strcmp(argv[1], "--record-session") == 0) return record_session();
    return check::run(argc, argv);
}
