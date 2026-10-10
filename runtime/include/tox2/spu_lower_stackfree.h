#pragma once

// The host-stack-free lowering of SPU control flow, and the only header that knows its model: a
// transfer to another function or to a computed target is a musttail call, so the host stack holds
// the run loop frame, one generated frame and at most one hook frame. The macros assume the generated
// function's parameters `ctx` and `ls` (see spu_context.h) and that every generated function has the
// same signature, which musttail requires. A lowering with host calls would add its own header and
// emitter module next to this one.

#include "compiler.h"
#include "spu_context.h"

// Block entry (function entry or an entry-switch case). A cycle of tail calls between functions
// passes through no loop back edge and no dispatch, so the halt check lives here as well.
#define TOX2_SPU_SF_ENTER(addr)                               \
    do {                                                      \
        tox2SpuEnter(ctx, (addr));                            \
        if (__builtin_expect(tox2SpuHaltRequested(ctx), 0)) { \
            return;                                           \
        }                                                     \
    } while (0)

// Backward edge inside a function: where a thread stuck in a local loop notices a halt request.
#define TOX2_SPU_SF_LOOP(site)                                \
    do {                                                      \
        if (__builtin_expect(tox2SpuHaltRequested(ctx), 0)) { \
            ctx->jump_site = (site);                          \
            return;                                           \
        }                                                     \
    } while (0)

// Transfer to a dispatchable block of another function whose address is known at emit time. The
// block keeps a label directly in front of a tail call from taking the attribute (C23 parses
// "L: __attribute__((musttail)) return" as a label attribute and rejects it).
#define TOX2_SPU_SF_JUMP(fn, target)           \
    do {                                       \
        ctx->jump_target = (uint32_t)(target); \
        TOX2_MUSTTAIL return fn(ctx, ls);      \
    } while (0)

// Computed or out-of-image transfer. The emitter computes the target first, then writes the link
// register, then uses this macro (bisl $0,$0 targets the register it links into).
#define TOX2_SPU_SF_DISPATCH(site, target)             \
    do {                                               \
        ctx->jump_site = (site);                       \
        ctx->jump_target = (uint32_t)(target);         \
        TOX2_MUSTTAIL return tox2SpuDispatch(ctx, ls); \
    } while (0)

// The thread must leave (halt, a stop that ends the thread): back to the run loop.
#define TOX2_SPU_SF_LEAVE() \
    do {                    \
        return;             \
    } while (0)
