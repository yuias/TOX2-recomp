#pragma once

// Pure decoder of the RSX command FIFO: no threads, no guest memory. The consumer feeds it a word
// reader and a register-write sink. Header kinds and masks follow RPCS3 `Emu/RSX/gcm_enums.h`
// (`RSX_METHOD_*`) and `Emu/RSX/RSXFIFO.cpp` (`FIFO_control::read`, `thread::run_FIFO`).

#include <cstdint>
#include <string>
#include <string_view>

namespace dal {

// Header word layout. Methods are byte offsets of the register (RPCS3 stores them >> 2).
inline constexpr std::uint32_t kFifoOldJumpMask = 0xE0000003;
inline constexpr std::uint32_t kFifoOldJump = 0x20000000;
inline constexpr std::uint32_t kFifoOldJumpOffsetMask = 0x1FFFFFFC;
inline constexpr std::uint32_t kFifoNewJumpMask = 0xE0000003;
inline constexpr std::uint32_t kFifoNewJump = 0x00000001;
inline constexpr std::uint32_t kFifoNewJumpOffsetMask = 0xFFFFFFFC;
inline constexpr std::uint32_t kFifoCallMask = 0x00000003;
inline constexpr std::uint32_t kFifoCall = 0x00000002;
inline constexpr std::uint32_t kFifoCallOffsetMask = 0x1FFFFFFC;
inline constexpr std::uint32_t kFifoReturnMask = 0xFFFF0003;
inline constexpr std::uint32_t kFifoReturn = 0x00020000;
// Any of these bits set means "not a plain method header": flow control or malformed.
inline constexpr std::uint32_t kFifoNonMethodMask = 0xA0030003;
inline constexpr std::uint32_t kFifoNonIncrementMask = 0xE0030003;
inline constexpr std::uint32_t kFifoNonIncrement = 0x40000000;
inline constexpr std::uint32_t kFifoMethodMask = 0x0000FFFC;
inline constexpr std::uint32_t kFifoCountShift = 18;
inline constexpr std::uint32_t kFifoCountMask = 0x7FF;

// Method registers the consumer interprets (RPCS3 `gcm_enums.h`, shifted back to byte offsets).
inline constexpr std::uint32_t kMethodNv406eSetReference = 0x0050;
inline constexpr std::uint32_t kMethodNv406eSetContextDmaSemaphore = 0x0060;
inline constexpr std::uint32_t kMethodNv406eSemaphoreOffset = 0x0064;
inline constexpr std::uint32_t kMethodNv406eSemaphoreAcquire = 0x0068;
inline constexpr std::uint32_t kMethodNv406eSemaphoreRelease = 0x006C;
inline constexpr std::uint32_t kMethodNv4097SetContextDmaSemaphore = 0x01A4;
inline constexpr std::uint32_t kMethodNv4097SetContextDmaReport = 0x01A8;
inline constexpr std::uint32_t kMethodNv4097GetReport = 0x1800;
inline constexpr std::uint32_t kMethodNv4097SetSemaphoreOffset = 0x1D6C;
inline constexpr std::uint32_t kMethodNv4097BackEndWriteSemaphoreRelease = 0x1D70;
inline constexpr std::uint32_t kMethodNv4097TextureReadSemaphoreRelease = 0x1D74;
// Registers the argument census (rsx/rsx_census.h) interprets.
inline constexpr std::uint32_t kMethodNv4097SetSurfaceFormat = 0x0208;
inline constexpr std::uint32_t kMethodNv4097SetShaderProgram = 0x08E4;
inline constexpr std::uint32_t kMethodNv4097SetTransformProgram = 0x0B80;       // 32 registers
inline constexpr std::uint32_t kMethodNv4097SetVertexDataArrayOffset = 0x1680;  // 16 registers
inline constexpr std::uint32_t kMethodNv4097SetVertexDataArrayFormat = 0x1740;  // 16 registers
inline constexpr std::uint32_t kMethodNv4097SetBeginEnd = 0x1808;
inline constexpr std::uint32_t kMethodNv4097ArrayElement16 = 0x180C;
inline constexpr std::uint32_t kMethodNv4097ArrayElement32 = 0x1810;
inline constexpr std::uint32_t kMethodNv4097DrawArrays = 0x1814;
inline constexpr std::uint32_t kMethodNv4097InlineArray = 0x1818;
inline constexpr std::uint32_t kMethodNv4097SetIndexArrayAddress = 0x181C;
inline constexpr std::uint32_t kMethodNv4097SetIndexArrayDma = 0x1820;
inline constexpr std::uint32_t kMethodNv4097DrawIndexArray = 0x1824;
inline constexpr std::uint32_t kMethodNv4097SetTextureOffset = 0x1A00;  // 16 units, 0x20 bytes apart
inline constexpr std::uint32_t kMethodNv4097SetTransformProgramLoad = 0x1E9C;
inline constexpr std::uint32_t kMethodNv4097SetTransformProgramStart = 0x1EA0;
inline constexpr std::uint32_t kMethodNv4097SetTransformConstantLoad = 0x1EFC;
inline constexpr std::uint32_t kMethodNv4097SetTransformConstant = 0x1F00;  // 32 registers
// Registers the state decoders (rsx/rsx_decode.h) read.
inline constexpr std::uint32_t kMethodNv4097SetContextDmaColorB = 0x018C;
inline constexpr std::uint32_t kMethodNv4097SetContextDmaColorA = 0x0194;
inline constexpr std::uint32_t kMethodNv4097SetContextDmaZeta = 0x0198;
inline constexpr std::uint32_t kMethodNv4097SetSurfaceClipHorizontal = 0x0200;
inline constexpr std::uint32_t kMethodNv4097SetSurfaceClipVertical = 0x0204;
inline constexpr std::uint32_t kMethodNv4097SetSurfacePitchA = 0x020C;
inline constexpr std::uint32_t kMethodNv4097SetSurfaceColorAOffset = 0x0210;
inline constexpr std::uint32_t kMethodNv4097SetSurfaceZetaOffset = 0x0214;
inline constexpr std::uint32_t kMethodNv4097SetSurfaceColorBOffset = 0x0218;
inline constexpr std::uint32_t kMethodNv4097SetSurfacePitchB = 0x021C;
inline constexpr std::uint32_t kMethodNv4097SetSurfaceColorTarget = 0x0220;
inline constexpr std::uint32_t kMethodNv4097SetSurfacePitchZ = 0x022C;
inline constexpr std::uint32_t kMethodNv4097SetAlphaTestEnable = 0x0304;
inline constexpr std::uint32_t kMethodNv4097SetAlphaFunc = 0x0308;
inline constexpr std::uint32_t kMethodNv4097SetAlphaRef = 0x030C;
inline constexpr std::uint32_t kMethodNv4097SetBlendEnable = 0x0310;
inline constexpr std::uint32_t kMethodNv4097SetBlendFuncSfactor = 0x0314;
inline constexpr std::uint32_t kMethodNv4097SetBlendFuncDfactor = 0x0318;
inline constexpr std::uint32_t kMethodNv4097SetBlendColor = 0x031C;
inline constexpr std::uint32_t kMethodNv4097SetBlendEquation = 0x0320;
inline constexpr std::uint32_t kMethodNv4097SetColorMask = 0x0324;
inline constexpr std::uint32_t kMethodNv4097SetStencilTestEnable = 0x0328;
inline constexpr std::uint32_t kMethodNv4097SetStencilMask = 0x032C;
inline constexpr std::uint32_t kMethodNv4097SetShadeMode = 0x0368;
inline constexpr std::uint32_t kMethodNv4097SetLogicOpEnable = 0x0374;
inline constexpr std::uint32_t kMethodNv4097SetDepthBoundsTestEnable = 0x0380;
inline constexpr std::uint32_t kMethodNv4097SetClipMin = 0x0394;
inline constexpr std::uint32_t kMethodNv4097SetClipMax = 0x0398;
inline constexpr std::uint32_t kMethodNv4097SetScissorHorizontal = 0x08C0;
inline constexpr std::uint32_t kMethodNv4097SetScissorVertical = 0x08C4;
inline constexpr std::uint32_t kMethodNv4097SetViewportOffset = 0x0A20;  // 4 registers
inline constexpr std::uint32_t kMethodNv4097SetViewportScale = 0x0A30;   // 4 registers
inline constexpr std::uint32_t kMethodNv4097SetPolyOffsetFillEnable = 0x0A68;
inline constexpr std::uint32_t kMethodNv4097SetDepthFunc = 0x0A6C;
inline constexpr std::uint32_t kMethodNv4097SetDepthMask = 0x0A70;
inline constexpr std::uint32_t kMethodNv4097SetDepthTestEnable = 0x0A74;
inline constexpr std::uint32_t kMethodNv4097SetVertexDataScaled4sM = 0x0A80;  // 32 registers
inline constexpr std::uint32_t kMethodNv4097SetTexCoordControl = 0x0B40;      // 10 registers
inline constexpr std::uint32_t kMethodNv4097SetTwoSideLightEn = 0x142C;
inline constexpr std::uint32_t kMethodNv4097SetVertexData3fM = 0x1500;  // 64 registers
inline constexpr std::uint32_t kMethodNv4097SetVertexDataBaseOffset = 0x1738;
inline constexpr std::uint32_t kMethodNv4097SetVertexDataBaseIndex = 0x173C;
inline constexpr std::uint32_t kMethodNv4097SetCullFace = 0x1830;
inline constexpr std::uint32_t kMethodNv4097SetFrontFace = 0x1834;
inline constexpr std::uint32_t kMethodNv4097SetCullFaceEnable = 0x183C;
inline constexpr std::uint32_t kMethodNv4097SetTextureControl3 = 0x1840;  // 16 registers, 4 bytes apart
inline constexpr std::uint32_t kMethodNv4097SetVertexData2fM = 0x1880;    // 32 registers
inline constexpr std::uint32_t kMethodNv4097SetVertexData2sM = 0x1900;    // 16 registers
inline constexpr std::uint32_t kMethodNv4097SetVertexData4ubM = 0x1940;   // 16 registers
inline constexpr std::uint32_t kMethodNv4097SetVertexData4sM = 0x1980;    // 32 registers
// Per texture unit (0x20 bytes apart), relative to SET_TEXTURE_OFFSET.
inline constexpr std::uint32_t kMethodNv4097SetTextureFormat = 0x1A04;
inline constexpr std::uint32_t kMethodNv4097SetTextureAddress = 0x1A08;
inline constexpr std::uint32_t kMethodNv4097SetTextureControl0 = 0x1A0C;
inline constexpr std::uint32_t kMethodNv4097SetTextureControl1 = 0x1A10;
inline constexpr std::uint32_t kMethodNv4097SetTextureFilter = 0x1A14;
inline constexpr std::uint32_t kMethodNv4097SetTextureImageRect = 0x1A18;
inline constexpr std::uint32_t kMethodNv4097SetTextureBorderColor = 0x1A1C;
inline constexpr std::uint32_t kMethodNv4097SetVertexData4fM = 0x1C00;  // 64 registers: 16 slots x 4 words
inline constexpr std::uint32_t kMethodNv4097SetShaderControl = 0x1D60;
inline constexpr std::uint32_t kMethodNv4097SetZminMaxControl = 0x1D78;
inline constexpr std::uint32_t kMethodNv4097SetShaderWindow = 0x1D88;
inline constexpr std::uint32_t kMethodNv4097SetZstencilClearValue = 0x1D8C;
inline constexpr std::uint32_t kMethodNv4097SetColorClearValue = 0x1D90;
inline constexpr std::uint32_t kMethodNv4097ClearSurface = 0x1D94;
inline constexpr std::uint32_t kMethodNv4097SetRestartIndexEnable = 0x1DAC;
inline constexpr std::uint32_t kMethodNv4097SetRestartIndex = 0x1DB0;
inline constexpr std::uint32_t kMethodNv4097SetVertexData1fM = 0x1E40;  // 16 registers
inline constexpr std::uint32_t kMethodNv4097SetPointSize = 0x1EE0;
inline constexpr std::uint32_t kMethodNv4097SetVertexAttribInputMask = 0x1FF0;
inline constexpr std::uint32_t kMethodNv4097SetVertexAttribOutputMask = 0x1FF4;
inline constexpr std::uint32_t kMethodNv0039BufferNotify = 0x2328;
inline constexpr std::uint32_t kMethodNv3062SetContextDmaImageDestin = 0x6188;
inline constexpr std::uint32_t kMethodNv3062SetColorFormat = 0x6300;
inline constexpr std::uint32_t kMethodNv3062SetPitch = 0x6304;
inline constexpr std::uint32_t kMethodNv3062SetOffsetDestin = 0x630C;
inline constexpr std::uint32_t kMethodNv308aPoint = 0xA304;
inline constexpr std::uint32_t kMethodNv308aSizeOut = 0xA308;
inline constexpr std::uint32_t kMethodNv308aColor = 0xA400;  // 0x700 registers
inline constexpr std::uint32_t kMethodNv3089ImageIn = 0xC40C;
inline constexpr std::uint32_t kMethodGcmFlipHead = 0xE920;        // two registers, head 0 and 1
inline constexpr std::uint32_t kMethodGcmSetUserCommand = 0xEB00;  // two registers
inline constexpr std::uint32_t kMethodGcmFlipCommand = 0xFEAC;

class RsxSink {
public:
    virtual ~RsxSink() = default;
    // Reads the big-endian word at an IO offset; false when the page is unmapped.
    virtual bool readIo(std::uint32_t io, std::uint32_t& word) = 0;
    // One register write; `method` is a byte offset. false = "cannot complete now" (semaphore
    // acquire not satisfied, flip waiting for VBlank): the parser keeps the write and retries it on
    // the next run.
    virtual bool write(std::uint32_t method, std::uint32_t value) = 0;
    // Asked after each completed write; true ends the run right there with Budget, before any further
    // word is read, so the sink can do work outside the run that needs the state of exactly that write.
    virtual bool stopAfterWrite() { return false; }
};

enum class FifoStatus {
    Idle,     // get == put, or spinning on a jump to itself
    Waiting,  // a write returned false
    Budget,   // max_words consumed, or the sink asked to stop after a write
    Fault,    // malformed header or unmapped read; sticky until reset()
};

class FifoParser {
public:
    // Consumes words from `get` toward `put` (both IO offsets; the low two bits of put are ignored).
    // `get` always names the next unread word, so it already points past a write that returned false:
    // that write is kept inside the parser. After Fault `get` names the offending word.
    FifoStatus run(std::uint32_t& get, std::uint32_t put, std::uint32_t max_words, RsxSink& sink);

    std::string_view lastFault() const { return fault_; }
    // Method headers with at least one argument, for the census.
    std::uint64_t headersParsed() const { return headers_; }
    // Forgets a pending method, the call slot and the fault.
    void reset();
    // Where the guest-visible get register may point after a run that ended with `get`: the header
    // of an unfinished command, so the guest never sees a command as consumed before it completed.
    std::uint32_t commandGet(std::uint32_t get) const { return (remaining_ != 0 || pending_) ? command_get_ : get; }

private:
    FifoStatus fail(std::string message);

    // Arguments of the current method still to be read, and the register the next one goes to.
    std::uint32_t remaining_ = 0;
    std::uint32_t method_ = 0;
    std::uint32_t command_get_ = 0;  // IO offset of the current command's header
    bool increment_ = true;
    // A write the sink refused, retried first on the next run.
    bool pending_ = false;
    std::uint32_t pending_method_ = 0;
    std::uint32_t pending_value_ = 0;
    // Only one call level exists on the hardware.
    bool in_call_ = false;
    std::uint32_t return_get_ = 0;
    std::uint64_t headers_ = 0;
    std::string fault_;
    bool faulted_ = false;
};

}  // namespace dal
