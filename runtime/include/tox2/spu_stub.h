#pragma once

// Test support of the SPU stub runtime (library tox2_spu_stub). Deliberately not part of the contract
// in tox2/spu_context.h: a real runtime has no use for it.

#include "spu_context.h"

#ifdef __cplusplus
extern "C" {
#endif

// Selects the image that the stub's tox2SpuDispatch looks targets up in. The stub has no local-store
// image map, and the images of one program share LS addresses, so the caller names the image.
void tox2SpuStubSelectImage(const Tox2SpuImageInfo* image);

// Names the running test; the stub's fatal exits print it. The pointer must stay valid until it is
// replaced; nullptr clears it.
void tox2SpuStubSetLabel(const char* label);

#ifdef __cplusplus
}  // extern "C"
#endif
