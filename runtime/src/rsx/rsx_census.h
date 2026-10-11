#pragma once

// Opt-in census of the register arguments the RSX consumer is fed (tox2_game --rsx-census FILE). It
// answers "what does the game ask the GPU to do": surface and texture formats, vertex layouts,
// primitive types, draw sizes, render state, the fragment and vertex programs (dumped to disk) and
// the flip rate. The consumer calls the on*() hooks only while the census is enabled.
//
// Register numbers and bit layouts follow RPCS3 `Emu/RSX/gcm_enums.h` and `rsx_decode.h`; the
// fragment program layout follows `Program/RSXFragmentProgram.h` and
// `ProgramStateCache.cpp` (`get_fragment_program_ucode_size`), the vertex program layout
// `Program/RSXVertexProgram.h` and `NV47/HW/nv4097.cpp` (`set_transform_program`).

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "rsx/rsx_registers.h"

namespace tox2 {

inline constexpr std::uint32_t kRsxFpMaxInstructions = 2048;  // scan limit for a program without an end flag

// ---- pure decoders (unit tested) ----

// Register name, with the slot of an array register appended ("NV4097_SET_TEXTURE_FORMAT[3]"); hex
// when unknown.
std::string rsxMethodName(std::uint32_t method);

// Human-readable fields of an argument for the registers whose layout is known; empty otherwise.
std::string rsxDecodeArgument(std::uint32_t method, std::uint32_t value);

// "A8R8G8B8_LN", "DXT1_UN", ... from the 8-bit format field of SET_TEXTURE_FORMAT.
std::string rsxTextureFormatName(std::uint32_t format);

std::string rsxFpOpcodeName(std::uint32_t opcode);
std::string rsxVpVecOpcodeName(std::uint32_t opcode);
std::string rsxVpScaOpcodeName(std::uint32_t opcode);

std::uint64_t rsxHash64(const std::uint8_t* data, std::size_t size);  // FNV-1a

// Reads the word at a word index of a program; the value is already decoded from big endian.
using RsxFpWordReader = std::function<bool(std::uint32_t word_index, std::uint32_t& word)>;

struct RsxFpScan {
    std::uint32_t instructions = 0;    // 16-byte instructions, inline constants excluded
    std::uint32_t constant_words = 0;  // 16-byte constants stored inline after an instruction, in units of 4 words
    bool terminated = false;           // an instruction with the end flag was reached
    std::map<std::uint32_t, std::uint32_t> opcodes;
    std::vector<std::uint32_t> words;  // everything read, big-endian decoded
};

// Walks the program until its end flag. Inline constants (a source operand of register type
// constant) occupy the next 16 bytes. Stops without `terminated` when a read fails or after
// kRsxFpMaxInstructions.
RsxFpScan scanFragmentProgram(const RsxFpWordReader& read);

struct RsxVpScan {
    std::uint32_t instructions = 0;
    bool terminated = false;
    std::map<std::uint32_t, std::uint32_t> vec_opcodes, sca_opcodes;
};

// `ucode` holds kRsxVpInstructionCount * 4 words; the program runs from instruction `start` to the
// one with the end bit.
RsxVpScan scanVertexProgram(const std::uint32_t* ucode, std::uint32_t start);

// Scalar opcodes of the vertex program flow control (RSX_SCA_OPCODE_* in RPCS3 `RSXVertexProgram.h`).
inline constexpr std::uint32_t kRsxVpScaBra = 0x08;  // branch by address register
inline constexpr std::uint32_t kRsxVpScaBri = 0x09;  // branch by condition code
inline constexpr std::uint32_t kRsxVpScaCal = 0x0a;  // call
inline constexpr std::uint32_t kRsxVpScaCli = 0x0b;  // call by condition code
inline constexpr std::uint32_t kRsxVpScaRet = 0x0c;  // return
inline constexpr std::uint32_t kRsxVpScaBrb = 0x11;  // branch by boolean constant
inline constexpr std::uint32_t kRsxVpScaClb = 0x12;  // call by boolean constant

// Fields of one 4-word instruction that the flow control uses. The target and the boolean index
// overlap the source 2 fields, so an instruction with a flow-control opcode has no source 2.
inline std::uint32_t rsxVpScaOpcode(const std::uint32_t* instruction) { return (instruction[1] >> 27) & 0x1F; }
inline std::uint32_t rsxVpCondition(const std::uint32_t* instruction) { return (instruction[0] >> 10) & 7; }
inline bool rsxVpIsEnd(const std::uint32_t* instruction) { return (instruction[3] & 1) != 0; }
inline std::uint32_t rsxVpBranchTarget(const std::uint32_t* instruction) {
    return (((instruction[0] >> 23) & 1) << 9) | ((instruction[2] & 0x3F) << 3) | (instruction[3] >> 29);
}
inline void rsxVpSetBranchTarget(std::uint32_t* instruction, std::uint32_t target) {
    instruction[0] = (instruction[0] & ~(1u << 23)) | (((target >> 9) & 1) << 23);
    instruction[2] = (instruction[2] & ~0x3Fu) | ((target >> 3) & 0x3F);
    instruction[3] = (instruction[3] & 0x1FFFFFFFu) | ((target & 7) << 29);
}
inline std::uint32_t rsxVpBranchIndex(const std::uint32_t* instruction) { return (instruction[3] >> 23) & 0x1F; }
inline bool rsxVpBranchTrueSense(const std::uint32_t* instruction) { return ((instruction[3] >> 28) & 1) != 0; }
// True for the opcodes that carry a fixed target address (BRA takes its target from the address register).
inline bool rsxVpHasBranchTarget(std::uint32_t sca_opcode) {
    return sca_opcode == kRsxVpScaBri || sca_opcode == kRsxVpScaCal || sca_opcode == kRsxVpScaCli ||
           sca_opcode == kRsxVpScaBrb || sca_opcode == kRsxVpScaClb;
}

// Result of following the control flow of a vertex program.
struct RsxVpFlow {
    std::vector<std::uint8_t> reached;  // per instruction index (absolute), 1 when some path executes it
    std::uint32_t last = 0;             // highest reached index; the program occupies [entry, last]
    bool terminated = false;            // a reached instruction has the end bit
    bool bad_target = false;            // a branch or call leaves [entry, instruction_count)
    bool has_flow_control = false;      // a reached instruction is a branch, call or return
};

// Follows the program from `entry` through `instruction_count` instructions: falls through, takes
// the target of every branch (a conditional one also falls through), and runs a call's callee up to
// its return before continuing after the call. A program may therefore extend past its first end
// bit; the reference is `analyse_vertex_program` in RPCS3 `Program/ProgramStateCache.cpp`.
// BRA targets come from the address register at run time, so BRA is treated as falling through.
RsxVpFlow analyzeVertexProgramFlow(const std::uint32_t* ucode, std::uint32_t instruction_count, std::uint32_t entry);

// ---- collector ----

class RsxArgCensus {
public:
    // Reads a big-endian decoded word of RSX-visible memory: location 0 = local (video) memory,
    // 1 = main memory through the IO map; false when it is not readable.
    using MemoryReader = std::function<bool(std::uint32_t location, std::uint32_t offset, std::uint32_t& word)>;

    RsxArgCensus(std::filesystem::path json_path, std::filesystem::path dump_dir, MemoryReader reader,
                 std::uint64_t start_us);

    // Hot path, RSX thread: a register write that completed.
    void onWrite(std::uint32_t method, std::uint32_t value);
    void onFlip(std::uint64_t now_us);
    void onVBlank(std::uint64_t now_us);

    // The census as JSON text.
    std::string formatJson();
    // Writes the JSON file and the program dumps. Gives up (false) when the RSX thread keeps the lock.
    bool writeFiles();

    // Introspection for tests.
    std::uint64_t writesOf(std::uint32_t method);
    std::size_t distinctValuesOf(std::uint32_t method);
    std::uint64_t draws();
    std::size_t fragmentProgramCount();
    std::size_t vertexProgramCount();

private:
    struct ValueCounts {
        std::unordered_map<std::uint32_t, std::uint64_t> counts;
        std::uint64_t overflow = 0;  // writes of values beyond the distinct-value cap
    };
    struct MethodStat {
        std::uint64_t writes = 0;
        ValueCounts values;
    };
    struct FragmentProgram {
        std::uint32_t location = 0, offset = 0;
        RsxFpScan scan;
        std::vector<std::uint8_t> bytes;  // as stored in guest memory
        bool zero_filled = false;
        std::uint64_t uses = 0;
    };
    struct VertexProgram {
        std::uint32_t start = 0;
        RsxVpScan scan;
        std::vector<std::uint8_t> bytes;  // big-endian words
        std::uint64_t uses = 0;
    };

    void onBegin();
    void onDrawArrays(std::uint32_t value);
    void onDrawIndexArray(std::uint32_t value);
    void onVertexProgramWord(std::uint32_t method, std::uint32_t value);
    std::string snapshotFragmentProgram();
    std::string snapshotVertexProgram();
    void addText(const std::string& category, const std::string& key);
    void addSize(const std::string& category, std::uint32_t count);
    void bump(std::vector<std::uint32_t>& series, std::uint64_t now_us);
    std::size_t secondOf(std::uint64_t now_us) const;
    std::string formatJsonLocked();

    std::filesystem::path json_path_, dump_dir_;
    MemoryReader reader_;
    std::uint64_t start_us_;

    std::mutex mutex_;
    std::array<std::uint32_t, 0x4000> regs_{};  // latest value of every register
    std::unordered_map<std::uint32_t, MethodStat> methods_;
    std::map<std::string, std::map<std::string, std::uint64_t>> text_;
    std::map<std::uint64_t, FragmentProgram> fragment_programs_;
    std::map<std::uint64_t, VertexProgram> vertex_programs_;
    std::array<std::uint32_t, kRsxVpInstructionCount * 4> vp_ucode_{};
    std::uint64_t unreadable_fragment_programs_ = 0, unterminated_vertex_programs_ = 0;

    std::uint64_t draws_ = 0, frame_draws_ = 0, flips_ = 0, vblanks_ = 0;
    std::uint32_t primitive_ = 0;
    std::map<std::uint32_t, std::uint64_t> draws_per_frame_;
    std::vector<std::uint32_t> flips_per_second_, vblanks_per_second_, draws_per_second_;
};

}  // namespace tox2
