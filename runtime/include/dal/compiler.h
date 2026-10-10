#pragma once

#ifdef __cplusplus
#define DAL_MUSTTAIL [[clang::musttail]]
#else
#define DAL_MUSTTAIL __attribute__((musttail))
#endif
#define DAL_NOINLINE __attribute__((noinline))
