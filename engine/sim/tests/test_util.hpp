#pragma once

#include <cstdio>
#include <cstdlib>

// assert() отключается в Release (NDEBUG) -- этим тестам он не подходит.
#define WSIM_CHECK(cond)                                                            \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::fprintf(stderr, "CHECK FAILED: %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
            std::exit(1);                                                           \
        }                                                                           \
    } while (0)
