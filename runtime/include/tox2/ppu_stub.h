#pragma once

// Test support of the stub runtime (library tox2_ppu_stub). Deliberately not part of the contract
// in tox2/ppu_context.h: a real runtime has no use for it.

#ifdef __cplusplus
extern "C" {
#endif

// Forgets every registered function. tox2RegisterFuncs keeps duplicates and tox2Lookup returns the
// first registration of a guest address, so a harness that registers several unrelated programs
// at the same guest addresses must reset between them.
void tox2StubResetFuncs(void);

// Names the running test; the stub's fatal exits (unhandled instruction, trap, unknown target, HLE
// call) print it. The pointer must stay valid until it is replaced; nullptr clears it.
void tox2StubSetLabel(const char* label);

#ifdef __cplusplus
}  // extern "C"
#endif
