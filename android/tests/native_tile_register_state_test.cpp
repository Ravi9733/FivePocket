#include "native_tile_register_state.h"
#include "native_tile_register_writeback.h"
#include <array>
#include <cassert>
#include <cstdio>
using namespace recomp::gpu;
int main() {
    const auto packet=[](unsigned header,std::initializer_list<unsigned> words) {
        std::vector<std::uint8_t> b;const auto append=[&](unsigned w){for(unsigned shift:{24u,16u,8u,0u})b.push_back(w>>shift);};
        append(header);for(auto w:words)append(w);return b;
    };
    NativeTileRegisterState::Journal journal{{0xA31,{0x80010000,UINT32_MAX}},{0xC44,{256,256}},{0x2319,{0x111,15}}};
    auto first=NativeTileRegisterState::parse(packet(0x2319,{0x071D0000}));assert(first);
    first->apply(journal);assert(journal.at(0x2319).first==0x071D0000 && journal.at(0x2319).second==UINT32_MAX);
    const auto consume=[&] {const auto value=journal.at(0x2319).first;return std::array<std::uint8_t,4>{
        std::uint8_t(value),std::uint8_t(value>>8),std::uint8_t(value>>16),std::uint8_t(value>>24)};};
    const auto old=consume();assert((old==std::array<std::uint8_t,4>{0,0,0x1D,7}));
    auto changed=NativeTileRegisterState::parse(packet(0x2319,{0x05EA2000}));assert(changed);changed->apply(journal);
    assert(consume()!=old && (consume()==std::array<std::uint8_t,4>{0,0x20,0xEA,5}));
    assert(journal.at(0xA31).first==0x80010000 && journal.at(0xC44).second==256);
    auto group=NativeTileRegisterState::parse(packet(0x22319,{1,2,3}));assert(group && group->writes().size()==3);group->apply(journal);
    assert(journal.at(0x2319).first==1 && journal.at(0x231A).first==2 && journal.at(0x231B).first==3);
    const auto stable=journal;
    for(unsigned reg:{0x578u,0x5C8u,0xA2Fu,0xA30u,0xA31u,0xE00u,0xE40u,0x1923u,0xFFFFu,0x23A0u,0x5003u})
        assert(!NativeTileRegisterState::parse(packet(reg,{0})));
    // No partial apply when a sequence crosses from allowed state into a gap.
    assert(!NativeTileRegisterState::parse(packet(0x1231B,{1,2})) && journal==stable);
    assert(!NativeTileRegisterState::parse(packet(0x2319|0x8000,{1})));
    assert(!NativeTileRegisterState::parse(packet(0xC0002319,{1})));
    assert(!NativeTileRegisterState::parse(packet(0x12319,{1})));
    auto valid=packet(0x2319,{0x7FC00001});assert(NativeTileRegisterState::parse(valid));
    for(unsigned n=0;n<valid.size();++n)assert(!NativeTileRegisterState::parse(std::span(valid).first(n)));
    valid.push_back(0);assert(!NativeTileRegisterState::parse(valid));
    const auto copyBytes=packet(0xC0015501,{0x2319,0x071D0000});
    const auto copy=NativeTileRegisterWriteback::parse(copyBytes);assert(copy);
    DrawBinState bins{{UINT32_MAX,0,0x80000003,0},7};
    std::array<std::uint8_t,4> destination{};unsigned reads=0,writes=0;
    const auto read=[&](unsigned reg)->std::optional<unsigned> {
        ++reads;const auto found=journal.find(reg);
        if(found==journal.end() || found->second.second!=UINT32_MAX)return std::nullopt;
        return found->second.first;
    };
    const auto publish=[&](unsigned address,const auto& bytes) {
        assert(address==0x071D0000);++writes;destination=bytes;return true;
    };
    // Exercise the actual shared consumer, not a lambda duplicating its logic.
    first->apply(journal);assert(copy->execute(bins,read,publish) && destination==old);
    changed->apply(journal);assert(copy->execute(bins,read,publish) && destination==consume() && destination!=old);
    const auto preserved=destination;journal.erase(0x2319);
    assert(!copy->execute(bins,read,publish) && destination==preserved && writes==2);
    journal[0x2319]={123,15};assert(!copy->execute(bins,read,publish) && writes==2);
    bins.words[0]=4;const auto previousReads=reads;
    assert(copy->execute(bins,read,publish) && reads==previousReads && writes==2);
    bins.words[0]=UINT32_MAX;bins.knownWords=3;
    assert(!copy->execute(bins,read,publish) && reads==previousReads && writes==2);
    bins.knownWords=7;first->apply(journal);
    assert(!copy->execute(bins,read,[&](auto,const auto&){return false;}));
    for(unsigned n=0;n<copyBytes.size();++n)assert(!NativeTileRegisterWriteback::parse(std::span(copyBytes).first(n)));
    for(auto bytes:{packet(0xC0015500,{0x2319,0x071D0000}),packet(0xC0015501,{0x578,0x071D0000}),
                    packet(0xC0015501,{0x2319,0x071D0001}),packet(0xC0015501,{0x2319,0x20000000})})
        assert(!NativeTileRegisterWriteback::parse(bytes));
    std::puts("NATIVE_REGISTER_WRITEBACK_CONNECTION_PASS current_producer=1 changed_value_propagated=1 missing_and_partial_rejected=1 skipped_no_read_or_write=1 unknown_predicate_rejected=1 gpu_execution=0");
    std::puts("NATIVE_TILE_REGISTER_STATE_PASS exact_raw_values=1 changing_producer_reaches_consumer=1 partial_known_preserved=1 side_effect_registers_rejected=1 malformed_and_cross_gap_rejected=1 gpu_execution=0");
}
