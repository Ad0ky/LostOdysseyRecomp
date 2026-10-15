// TranslatedShader::positionConstantSlots on synthetic Xenos vertex shaders: the
// constant slots that can reach oPos, followed per register component in program
// order. Camera-static motion replay skipping compares only these slots.
#include <gpu/shader/xenos_translator.h>
#include <gpu/shader/xenos_shader_code.h>
#include <array>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <stdexcept>
#include <vector>
using namespace xenos;

static unsigned checks = 0;
static void Require(bool ok, const char* why) { ++checks; if (!ok) throw std::runtime_error(why); }

struct Program {
    std::vector<uint32_t> code = std::vector<uint32_t>(3, 0); // one control-flow pair
    void Instruction(const void* bits) {
        const size_t at = code.size();
        code.resize(at + 3);
        std::memcpy(code.data() + at, bits, 12);
    }
    // Two execs: the first covers up to six instructions from address 1.
    void Finish(uint32_t fetchSequence) {
        const uint32_t count = uint32_t(code.size() / 3 - 1);
        ControlFlowExecInstruction cf{};
        cf.address = 1; cf.count = std::min<uint32_t>(count, 6); cf.sequence = fetchSequence;
        cf.opcode = count > 6 ? ControlFlowOpcode::Exec : ControlFlowOpcode::ExecEnd;
        std::memcpy(code.data(), &cf, 6);
        if (count > 6) {
            cf = {}; cf.address = 7; cf.count = count - 6; cf.opcode = ControlFlowOpcode::ExecEnd;
            std::memcpy(reinterpret_cast<uint8_t*>(code.data()) + 6, &cf, 6);
        }
    }
};

static VertexFetchInstruction Fetch(uint32_t dst) {
    VertexFetchInstruction f{};
    f.opcode = FetchOpcode::VertexFetch; f.mustBeOne = 1; f.dstRegister = dst; f.srcRegister = 0;
    f.dstSwizzle = 0 | (1 << 3) | (2 << 6) | (uint32_t(FetchDestinationSwizzle::One) << 9);
    f.format = uint32_t(VertexFormat::k_32_32_32_FLOAT); f.stride = 3;
    return f;
}
// dst = a OP b with a a register and b a register or constant; export to oPos lane when lane >= 0.
static AluInstruction Vector(AluVectorOpcode op, uint32_t dst, uint32_t a, uint32_t b, bool bConstant,
    uint32_t mask = 0xF, int positionLane = -1) {
    AluInstruction i{};
    i.vectorOpcode = op; i.scalarOpcode = AluScalarOpcode::RetainPrev;
    i.vectorDest = dst; i.vectorWriteMask = mask;
    i.src1Register = a; i.src1Select = 1;
    i.src2Register = b; i.src2Select = bConstant ? 0 : 1;
    i.src3Select = 1;
    if (positionLane >= 0) {
        i.exportData = 1; i.vectorDest = uint32_t(ExportRegister::VSPosition); i.vectorWriteMask = 1u << positionLane;
    }
    return i;
}
static std::array<uint64_t, 4> Slots(std::initializer_list<unsigned> slots) {
    std::array<uint64_t, 4> out{};
    for (unsigned s : slots) out[s / 64] |= uint64_t(1) << (s % 64);
    return out;
}
static TranslatedShader Translate(Program& p, uint32_t fetchSequence) {
    p.Finish(fetchSequence);
    return TranslateShader(p.code.data(), uint32_t(p.code.size()), false);
}
// Lighting reuses r5 for an eye vector, then the position: only c4 and c7..c10 reach oPos.
static void RegisterReuse() {
    Program p;
    const auto fetch = Fetch(1); p.Instruction(&fetch);
    const auto eye = Vector(AluVectorOpcode::Add, 5, 1, 11, true); p.Instruction(&eye);
    auto toInterpolator = Vector(AluVectorOpcode::Add, 0, 5, 5, false);
    toInterpolator.exportData = 1; p.Instruction(&toInterpolator);
    const auto world = Vector(AluVectorOpcode::Mul, 5, 1, 4, true); p.Instruction(&world);
    for (int lane = 0; lane < 4; ++lane) {
        const auto row = Vector(AluVectorOpcode::Dp4, 0, 5, 7 + lane, true, 1, lane); p.Instruction(&row);
    }
    const auto t = Translate(p, 1);
    Require(t.errors.empty(), "synthetic shader translates");
    Require(t.positionConstantsKnown, "straight-line position flow is known");
    Require(t.positionConstantSlots == Slots({4, 7, 8, 9, 10}), "a reused register drops the eye constant");
}
// A predicated write keeps the old value's slots and adds the predicate's.
static void Predicated() {
    Program p;
    const auto fetch = Fetch(1); p.Instruction(&fetch);
    const auto world = Vector(AluVectorOpcode::Mul, 5, 1, 4, true); p.Instruction(&world);
    AluInstruction setp{};
    setp.vectorOpcode = AluVectorOpcode::Add; setp.vectorWriteMask = 0;
    setp.scalarOpcode = AluScalarOpcode::SetpGt; setp.src3Register = 13; setp.src3Select = 0;
    p.Instruction(&setp);
    auto offset = Vector(AluVectorOpcode::Mul, 5, 1, 14, true);
    offset.isPredicated = 1; offset.predicateCondition = 1; p.Instruction(&offset);
    for (int lane = 0; lane < 4; ++lane) {
        const auto row = Vector(AluVectorOpcode::Dp4, 0, 5, 7 + lane, true, 1, lane); p.Instruction(&row);
    }
    const auto t = Translate(p, 1);
    Require(t.positionConstantsKnown, "predicated position flow is known");
    Require(t.positionConstantSlots == Slots({4, 7, 8, 9, 10, 13, 14}), "predicate and both writes reach oPos");
}
// Relative addressing that reaches oPos leaves the flow unknown.
static void Relative() {
    Program p;
    const auto fetch = Fetch(1); p.Instruction(&fetch);
    auto world = Vector(AluVectorOpcode::Mul, 5, 1, 4, true);
    world.const0Relative = 1; world.constAddressRegisterRelative = 1; p.Instruction(&world);
    for (int lane = 0; lane < 4; ++lane) {
        const auto row = Vector(AluVectorOpcode::Dp4, 0, 5, 7 + lane, true, 1, lane); p.Instruction(&row);
    }
    const auto t = Translate(p, 1);
    Require(t.usesRelativeConstants, "relative addressing is reported");
    Require(!t.positionConstantsKnown, "relative constants reaching oPos are unknown");
}
// Lane-wise ops keep components apart: only lane x of r6 depends on c20.
static void Lanes() {
    Program p;
    const auto fetch = Fetch(1); p.Instruction(&fetch);
    const auto base = Vector(AluVectorOpcode::Mul, 6, 1, 4, true); p.Instruction(&base);
    const auto x = Vector(AluVectorOpcode::Add, 6, 1, 20, true, 0b0001); p.Instruction(&x);
    auto y = Vector(AluVectorOpcode::Add, 0, 6, 6, false, 0b0010, 1);
    y.src1Swizzle = y.src2Swizzle = 0; p.Instruction(&y);
    const auto t = Translate(p, 1);
    Require(t.positionConstantsKnown, "lane flow is known");
    Require(t.positionConstantSlots == Slots({4}), "oPos.y from r6.y does not depend on the c20 written to r6.x");
}
int main() {
    try {
        RegisterReuse(); Predicated(); Relative(); Lanes();
        std::printf("PASS: %u position flow checks\n", checks);
        return 0;
    } catch (const std::exception& e) {
        std::printf("FAIL: %s\n", e.what());
        return 1;
    }
}
