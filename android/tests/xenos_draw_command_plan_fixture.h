#pragma once
#include "xenos_draw_command_plan.h"

namespace recomp::gpu::test {
// Deterministic BE-word test encoding: not a runtime, file or wire ABI. Contains
// source bytes, so real-capture results must remain in ignored local-analysis/.
inline std::vector<std::uint32_t> encodeDrawCommandPlan(const DrawCommandPlan& p) {
    std::vector<std::uint32_t> out;
    const auto word = [&](std::uint32_t v) { out.push_back(v); };
    const auto registers = [&](const DrawRegisterCandidates& regs) {
        word(static_cast<std::uint32_t>(regs.size()));
        for (const auto& [index,r] : regs) {
            word(index); word(r.bits.value); word(r.bits.known); word(r.lastAssignmentOffset);
        }
    };
    const auto bins = [&](const DrawBinState& state) {
        word(state.knownWords); for (auto v : state.words) word(v);
    };
    const auto bytes = [&](const std::vector<std::uint8_t>& b) {
        word(static_cast<std::uint32_t>(b.size()));
        for (std::size_t i = 0; i < b.size(); i += 4)
            word((std::uint32_t{b[i]}<<24) | (std::uint32_t{b[i+1]}<<16) | (std::uint32_t{b[i+2]}<<8) | b[i+3]);
    };
    word(static_cast<std::uint32_t>(p.commands.words().size()));
    for (auto v : p.commands.words()) word(v);
    word(static_cast<std::uint32_t>(p.actions.size()));
    for (const auto& a : p.actions) { word(static_cast<std::uint32_t>(a.kind)); word(a.offsetWords); word(a.priorDraws); word(a.detailIndex); }
    word(static_cast<std::uint32_t>(p.shaders.size()));
    for (const auto& s : p.shaders) { word(s.offsetWords); word(s.stage); word(s.physicalAddress); word(s.immediate); bytes(s.bigEndianCode); }
    word(static_cast<std::uint32_t>(p.constants.size()));
    for (const auto& c : p.constants) { word(c.offsetWords); word(c.physicalAddress); word(c.firstRegister); bytes(c.bigEndianWords); }
    word(static_cast<std::uint32_t>(p.draws.size()));
    for (const auto& d : p.draws) {
        word(d.offsetWords); word(d.opcode); word(d.indexCount); word(d.primitive);
        for (auto s : d.shaders) word(s);
        word(d.predicate.predicated); bins(d.predicate.state); registers(d.registers);
    }
    word(static_cast<std::uint32_t>(p.waits.size()));
    for (const auto& w : p.waits) { word(w.offsetWords); word(w.priorDraws); registers(w.registers); }
    registers(p.finalRegisters); bins(p.finalBins);
    return out;
}
}
