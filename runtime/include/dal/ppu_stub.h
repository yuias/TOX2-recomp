#pragma once

// Test support of the stub runtime (library dal_ppu_stub). Deliberately not part of the contract
// in dal/ppu_context.h: a real runtime has no use for it.

#ifdef __cplusplus
extern "C" {
#endif

// Forgets every registered function. dalRegisterFuncs keeps duplicates and dalLookup returns the
// first registration of a guest address, so a harness that registers several unrelated programs
// at the same guest addresses must reset between them.
void dalStubResetFuncs(void);

// Names the running test; the stub's fatal exits (unhandled instruction, trap, unknown target, HLE
// call) print it. The pointer must stay valid until it is replaced; nullptr clears it.
void dalStubSetLabel(const char* label);

#ifdef __cplusplus
}  // extern "C"
#endif
