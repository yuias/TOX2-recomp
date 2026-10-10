// Compiled with -ffreestanding: the SPU contract headers must parse without the C library headers.

#include "tox2/spu_context.h"
#include "tox2/spu_lower_stackfree.h"
#include "tox2/spu_ops.h"

TOX2_SPU_FUNC(spuHeaderFreestandingProbe) {
    TOX2_SPU_SF_ENTER(0x100);
    ctx->gpr[3] = tox2SpuLoadQ(ls, 0x110);
    tox2SpuStoreQ(ls, 0x120, ctx->gpr[3]);
    if (tox2SpuPreferred(ctx->gpr[3]) == 0) TOX2_SPU_SF_LEAVE();
    TOX2_SPU_SF_DISPATCH(0x104, ctx->jump_target);
}
