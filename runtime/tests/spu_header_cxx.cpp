// Compiled as C++26: the SPU contract headers must parse as C++ and the lowering macros must work
// with [[clang::musttail]].

#include "tox2/spu_context.h"
#include "tox2/spu_lower_stackfree.h"

static_assert(sizeof(Tox2SpuContext) % 16 == 0);
static_assert(kTox2SpuNoExit == 0xffffffffu);

extern "C" {
TOX2_SPU_FUNC(spuHeaderCxxCallee) { ctx->gpr[4] = tox2SpuLoadQ(ls, kTox2SpuLsSize + 0x10); }

TOX2_SPU_FUNC(spuHeaderCxxProbe) {
    TOX2_SPU_SF_ENTER(0x100);
    if (tox2SpuPreferred(ctx->gpr[3]) == 0) TOX2_SPU_SF_LEAVE();
    TOX2_SPU_SF_LOOP(0x108);
    if (tox2SpuPreferred(ctx->gpr[3]) == 1) TOX2_SPU_SF_JUMP(spuHeaderCxxCallee, 0x200);
    TOX2_SPU_SF_DISPATCH(0x10c, ctx->jump_target);
}
}
