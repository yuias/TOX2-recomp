#pragma once

#ifdef __cplusplus
#define TOX2_MUSTTAIL [[clang::musttail]]
#else
#define TOX2_MUSTTAIL __attribute__((musttail))
#endif
#define TOX2_NOINLINE __attribute__((noinline))
