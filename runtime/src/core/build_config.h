#pragma once

// TOX2_RSX_WRITE_WATCH (0 or 1) comes from the build (cmake/Runtime.cmake) for every target that links the runtime
// core. Structures and a virtual function differ by its value, so a translation unit that guessed it would break the
// one-definition rule.
#if !defined(TOX2_RSX_WRITE_WATCH)
#error "TOX2_RSX_WRITE_WATCH is not defined: link tox2_runtime_core or add its compile definition"
#endif
