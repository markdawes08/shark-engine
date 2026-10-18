#include "../include/api.hpp"

#if SHARK_HEADER_LEAK == 1
#include "../../client/leak.hpp"
#elif SHARK_HEADER_LEAK == 2
#include "generated.hpp"
#endif

int core_value() {
    return 7;
}
