#pragma once
// Xenos shader microcode: decoding into a structured form, and a
// disassembler. Bit layouts are the hardware's: control flow in 48-bit pairs,
// ALU and fetch instructions in 96-bit (three-dword) words.
#include <cstdint>
#include <string>
#include <vector>

namespace regpu
{
namespace ucode
{
enum class CfOp : uint32_t
{
    Nop = 0, Exec = 1, ExecEnd = 2, CondExec = 3, CondExecEnd = 4, CondExecPred = 5, CondExecPredEnd = 6,
    LoopStart = 7, LoopEnd = 8, CondCall = 9, Return = 10, CondJmp = 11, Alloc = 12,
    CondExecPredClean = 13, CondExecPredCleanEnd = 14, MarkVsFetchDone = 15,
};

struct Cf
{
    uint32_t lo = 0, hi = 0; // 48 bits
    CfOp op() const { return CfOp((hi >> 12) & 0xF); }
    // Exec family.
    uint32_t address() const { return lo & 0xFFF; }
    uint32_t count() const { return (lo >> 12) & 7; }
    uint32_t sequence() const { return (lo >> 16) & 0xFFF; }
    // Cond exec: bool constant index and condition.
    uint32_t boolAddress() const { return (lo >> 16) & 0xFF; } // for CondExec: bits 16-23 of word 0
    bool condition() const { return (hi >> 10) & 1; }
    // Alloc.
    uint32_t allocSize() const { return lo & 7; }
    uint32_t allocType() const { return (hi >> 9) & 3; }
    // Jumps, calls and loops: the target CF index (loop start: the index to skip to, normally
    // after the loop end; loop end: the start of the body, normally after the loop start).
    uint32_t target() const { return lo & 0x1FFF; }
    // CondJmp / CondCall: taken unconditionally, or on the predicate instead of a bool constant.
    bool isUnconditional() const { return (lo >> 13) & 1; }
    bool isPredicated() const { return (lo >> 14) & 1; }
    // CondJmp / CondCall / LoopEnd: bool constant index (jump, call) and the required value.
    uint32_t cfBoolAddress() const { return (hi >> 2) & 0xFF; }
    // LoopStart / LoopEnd: the loop constant (i0-i31) with count, start and step.
    uint32_t loopId() const { return (lo >> 16) & 0x1F; }
    // LoopStart: keep the iterator of the previous loop at this depth instead of restarting it.
    bool isRepeat() const { return (lo >> 13) & 1; }
    // LoopEnd: leave the loop when the predicate equals condition().
    bool isPredicatedBreak() const { return (lo >> 21) & 1; }
    bool IsExec() const;
    bool IsEnd() const;
};

struct Source
{
    uint32_t reg;     // register or constant index
    bool isConstant;
    bool negate;
    bool abs;
    uint8_t swizzle[4]; // component per channel, 0-3
    uint8_t relative;   // constants: 0 absolute, 1 + a0, 2 + aL (loop index)
    bool tempRelative;  // temporaries: index + aL
};

struct Alu
{
    uint32_t vectorOp, scalarOp;
    uint32_t vectorDest, scalarDest;
    bool vectorDestRelative; // + aL (not for exports)
    uint32_t vectorMask, scalarMask;
    bool exportData;
    bool scalarDestRelative;
    uint32_t src3RawReg; // for the constant/temp scalar ops (mulsc etc.)
    bool src3IsTemp;
    uint32_t src3RawSwizzle;
    bool vectorClamp, scalarClamp;
    bool predicated, predCondition;
    // mulsc / addsc / subsc: abs_constants applies to both of their operands, and the
    // constant's addressing (0 absolute, 1 + a0, 2 + aL) is that of a third constant operand.
    bool absConstants;
    uint8_t scalarConstRelative;
    Source src[3];
};

struct Fetch
{
    uint32_t opcode; // 0 vertex, 1 texture, other: texture queries
    uint32_t dest, src;
    bool destRelative, srcRelative; // register index + aL
    uint32_t destSwizzle; // 12 bits: 3 per channel
    uint32_t constIndex;  // vertex: fetch constant (index*3+sel); texture: slot
    // Vertex fetch.
    uint32_t format, stride;
    int32_t offset;      // dwords, signed
    bool isSigned, isNormalized, isMini;
    bool signedNoZero;   // signed normalized as (2x + 1) / (2^n - 1) instead of clamping -2^(n-1) to -1
    int32_t expAdjust;   // result scaled by 2^expAdjust
    bool roundIndex;     // index rounded to nearest instead of truncated
    // Texture fetch.
    uint32_t srcSwizzle; // 6 bits for texture (3 x 2)
    bool denormalized;   // coordinates in texels
    uint32_t dimension;
    float lodBias;       // texture fetch LOD bias (instruction)
    float offsetX = 0, offsetY = 0, offsetZ = 0; // texture fetch texel offsets (signed 5 bits, half texels)
    // Filter overrides (0 point, 1 linear, 2 base map (mip only), 3 from the fetch constant).
    uint32_t magFilter = 3, minFilter = 3, mipFilter = 3;
    uint32_t anisoFilter = 7; // 7: from the fetch constant
    bool useComputedLod = true, useRegisterLod = false, useRegisterGradients = false;
    bool predicated, predCondition;
};

struct Instruction
{
    bool isFetch;
    Alu alu;
    Fetch fetch;
};

struct Shader
{
    bool pixel = false;
    std::vector<Cf> cf;
    std::vector<uint32_t> words; // host-order dwords
    bool valid = false;
};

Shader Parse(const uint32_t* microcodeBE, uint32_t dwords, bool pixel);
// Control flow is parsed up to the first instruction slot, not just to the first end: called
// subroutines sit after it.
// Dwords up to the last instruction slot the program uses. Loaded shaders carry a
// per-load stamp after that, so hashes cover only this much. Returns dwords when the
// control flow has calls or jumps or doesn't parse.
uint32_t UsedDwords(const uint32_t* microcodeBE, uint32_t dwords);
Alu DecodeAlu(const uint32_t* words);
Fetch DecodeFetch(const uint32_t* words);
std::string Disassemble(const Shader& shader);
const char* VectorOpName(uint32_t op);
const char* ScalarOpName(uint32_t op);
}
} // namespace regpu
