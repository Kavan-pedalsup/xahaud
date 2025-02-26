/**
 *
 */
#include "hookapi.h"
// #define DEBUG 1
// #define SBUF(str) (uint32_t)(str), sizeof(str)
// #define TRACESTR(v) if (DEBUG) trace((uint32_t)(#v), (uint32_t)(sizeof(#v) - 1), (uint32_t)(v), sizeof(v), 0);
// // hook developers should use this guard macro, simply GUARD(<maximum iterations>)
// #define GUARD(maxiter) _g((1ULL << 31U) + __LINE__, (maxiter)+1)
// #define GUARDM(maxiter, n) _g(( (1ULL << 31U) + (__LINE__ << 16) + n), (maxiter)+1)

// hooks-cli compile-c contracts/custom.c build

int64_t methodCreate(uint32_t reserved) {
    TRACESTR("somefunc.c: Called.");
    for (int i = 0; GUARD(10), i < 10; i++) {
        TRACESTR("somefunc.c: Iterating.");
    }
    return accept(SBUF("somefunc.c: Finished."), __LINE__);
}