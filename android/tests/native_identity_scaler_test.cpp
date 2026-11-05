#include "native_identity_scaler.h"
#include "native_scaler_command.h"
#include "xenos_command_stream.h"
#include "xenos_native_state_batch.h"
#include <algorithm>
#include <cassert>
#include <cstdio>

using recomp::gpu::NativeIdentityScalerCandidate;
using recomp::gpu::NativeIdentityScalerPlan;
int main() {
    using Candidate = NativeIdentityScalerCandidate;
    Candidate::Inputs inputs{{0, (1280U << 16) | 720, 0, (1280U << 16) | 720, (1280U << 16) | 720, 7, 7}, {}};
    std::array<std::uint8_t, Candidate::RecordBytes + 2> storage{};
    auto* bytes = storage.data() + 1; // Decoding must not require host alignment.
    const auto put = [&](std::size_t word, std::uint32_t value) {
        for (unsigned lane = 0; lane < 4; ++lane) bytes[word * 4 + lane] = value >> (24 - lane * 8);
    };
    for (unsigned axis = 0; axis < 2; ++axis) {
        const unsigned taps = axis ? 10 : 5;
        const auto base = axis * Candidate::AxisWords;
        put(base, taps); put(base + 1, 0x3D800000); put(base + 162, 0x80000);
        for (unsigned i = (taps - 1) * 8; i < (taps + 1) * 8; ++i) put(base + 2 + i, 512);
    }
    // Deliberately invented nonzero control values: the parser cannot present
    // them as interpreted hardware state. Every bit must remain in the copy.
    for (unsigned i = 0; i < Candidate::ControlWords; ++i) put(326 + i, 0xABC00000 + i);
    const auto original = storage;
    const auto candidate = Candidate::inspect(inputs, bytes, Candidate::RecordBytes);
    assert(candidate && candidate->matches(bytes, Candidate::RecordBytes));
    assert(!NativeIdentityScalerPlan::make(*candidate)); // An input candidate is NOT native-policy admission.
    for (unsigned i = 0; i < Candidate::ControlWords; ++i)
        assert(candidate->unresolvedControls()[i] == 0xABC00000 + i);
    for (auto size : {0U, 4U, 1404U, 1409U, 1412U}) {
        assert(!Candidate::inspect(inputs, bytes, size) && !candidate->matches(bytes, size));
    }
    assert(!Candidate::inspect(inputs, nullptr, Candidate::RecordBytes));
    assert(!candidate->matches(nullptr, Candidate::RecordBytes));
    const auto* overflow = reinterpret_cast<const std::uint8_t*>(UINTPTR_MAX - Candidate::RecordBytes + 1);
    assert(!Candidate::inspect(inputs, overflow, Candidate::RecordBytes) && !candidate->matches(overflow, Candidate::RecordBytes));
    unsigned requestBits = 0, coefficientBits = 0, controlBits = 0;
    for (std::size_t word = 0; word < inputs.geometryAndFilters.size(); ++word) for (unsigned bit = 0; bit < 32; ++bit) {
        auto changed = inputs; changed.geometryAndFilters[word] ^= 1U << bit;
        assert(!Candidate::inspect(changed, bytes, Candidate::RecordBytes)); ++requestBits;
    }
    for (std::size_t word = 0; word < inputs.filterParameterBits.size(); ++word) for (unsigned bit = 0; bit < 32; ++bit) {
        auto changed = inputs; changed.filterParameterBits[word] ^= 1U << bit;
        assert(!Candidate::inspect(changed, bytes, Candidate::RecordBytes)); ++requestBits;
    }
    for (std::size_t byte = 0; byte < Candidate::RecordBytes; ++byte) for (unsigned bit = 0; bit < 8; ++bit) {
        bytes[byte] ^= 1U << bit;
        assert(!candidate->matches(bytes, Candidate::RecordBytes));
        const auto changed = Candidate::inspect(inputs, bytes, Candidate::RecordBytes);
        if (byte < 326 * 4) { assert(!changed); ++coefficientBits; }
        else {
            assert(changed && changed->matches(bytes, Candidate::RecordBytes));
            assert(changed->unresolvedControls() != candidate->unresolvedControls()); ++controlBits;
        }
        bytes[byte] ^= 1U << bit;
    }
    assert(storage == original && requestBits == 416 && coefficientBits == 10432 && controlBits == 832);
    const auto expected = NativeIdentityScalerPlan::expectedControls();
    for (unsigned i = 0; i < expected.size(); ++i) put(326 + i, expected[i]);
    const auto full = Candidate::inspect(inputs, bytes, Candidate::RecordBytes); assert(full);
    const auto plan = NativeIdentityScalerPlan::make(*full); assert(plan);
    assert(plan->candidate().matches(bytes, Candidate::RecordBytes));
    // Independent dimensional/phase checks, not just builder equality.
    assert(expected[4] == 1280 && expected[9] == 1280 && expected[10] == 720 && expected[13] == 0x050002D0);
    assert(expected[15] == 0x904 && expected[19] == 0x60000 && expected[23] == 0x38000 && expected[25] == 319);
    unsigned rejectedControlBits = 0;
    for (std::size_t byte = 326 * 4; byte < Candidate::RecordBytes; ++byte) for (unsigned bit = 0; bit < 8; ++bit) {
        bytes[byte] ^= 1U << bit;
        const auto changed = Candidate::inspect(inputs, bytes, Candidate::RecordBytes); assert(changed);
        assert(!NativeIdentityScalerPlan::make(*changed) && !plan->candidate().matches(bytes, Candidate::RecordBytes));
        bytes[byte] ^= 1U << bit; ++rejectedControlBits;
    }
    for (unsigned y = 0; y < 720; ++y) for (unsigned x = 0; x < 1280; ++x) {
        const auto offset = plan->rgbaSourceOffset(x, y); assert(offset && *offset == (y * 1280 + x) * 4);
    }
    for (auto x : {1280U, UINT32_MAX}) assert(!plan->rgbaSourceOffset(x, 0));
    for (auto y : {720U, UINT32_MAX}) assert(!plan->rgbaSourceOffset(0, y));
    assert(rejectedControlBits == 832);
    using Command=recomp::gpu::NativeScalerCommand;
    const Command::Identity id{0x0102030405060708,43,0x64001000};
    const auto command=Command::make(id,*plan);assert(command);
    assert(command->matches(command->bytes().data(),Command::Bytes));
    const auto& encoded=command->bytes();
    const auto word=[&](unsigned i) {const auto* p=encoded.data()+i*4;
        return (std::uint32_t{p[0]}<<24)|(std::uint32_t{p[1]}<<16)|(std::uint32_t{p[2]}<<8)|p[3];};
    assert(word(0)==0xFFFF5343 && word(1)==0x414C0001 && word(2)==64 && word(3)==1 &&
        word(4)==0x01020304 && word(5)==0x05060708 && word(6)==43 && word(7)==id.issuerThread &&
        word(8)==1280 && word(9)==720);
    for (unsigned i=49;i<64;++i) assert(word(i)==0);
    for (unsigned at=0;at<Command::Bytes;++at) for (unsigned bit=0;bit<8;++bit) {
        auto bad=encoded;bad[at]^=1U<<bit;assert(!command->matches(bad.data(),bad.size()));
    }
    for (unsigned size=0;size<Command::Bytes;++size) assert(!command->matches(encoded.data(),size));
    assert(!command->matches(encoded.data(),Command::Bytes+1) && !command->matches(nullptr,Command::Bytes));
    assert(!command->matches(reinterpret_cast<const std::uint8_t*>(UINTPTR_MAX-Command::Bytes+1),Command::Bytes));
    for (unsigned field=0;field<3;++field) {
        auto changed=id;
        if (!field) ++changed.ticket;else if (field==1) ++changed.previousReadWord;else changed.issuerThread+=4;
        const auto other=Command::make(changed,*plan);assert(other && !other->matches(encoded.data(),Command::Bytes));
    }
    assert(!Command::make({0,43,id.issuerThread},*plan) && !Command::make({1,4*1024*1024,id.issuerThread},*plan));
    assert(!Command::make({1,43,0},*plan) && !Command::make({1,43,id.issuerThread+1},*plan));
    recomp::gpu::Pm4Snapshot pm4;recomp::gpu::Pm4Error error;
    assert(!recomp::gpu::decodePm4(encoded.data(),encoded.size(),{4096,512,false},pm4,error));
    assert(error.failure==recomp::gpu::Pm4Failure::TruncatedPacket);
    std::vector<std::uint32_t> words;for (unsigned i=0;i<64;++i) words.push_back(word(i));
    recomp::gpu::NativeStateBatch batch;assert(!recomp::gpu::planNativeStateBatch(words,batch,error));
    std::fill_n(bytes, Candidate::RecordBytes, 0);
    assert(candidate->record()[0] == 5 && candidate->record()[163] == 10); // No retained input view.
    assert(candidate->unresolvedControls()[25] == 0xABC00019);
    assert(plan->candidate().record()[351] == 319);
    assert(command->plan().candidate().record()[351]==319);
    std::vector<std::uint8_t> allocation(8*1024*1024);
    assert(!command->uniqueOffset(allocation.data(),allocation.size()));
    for (unsigned offset:{0u,4u,1024u,8*1024*1024-Command::Bytes}) {
        std::copy(encoded.begin(),encoded.end(),allocation.begin()+offset);
        assert(command->uniqueOffset(allocation.data(),allocation.size())==offset);
        std::fill_n(allocation.data()+offset,Command::Bytes,0);
    }
    std::copy(encoded.begin(),encoded.end(),allocation.begin());
    std::copy(encoded.begin(),encoded.end(),allocation.begin()+Command::Bytes);
    assert(!command->uniqueOffset(allocation.data(),allocation.size()));
    assert(!command->uniqueOffset(allocation.data(),255) && !command->uniqueOffset(allocation.data(),257) &&
        !command->uniqueOffset(allocation.data(),8*1024*1024+4) && !command->uniqueOffset(nullptr,256));
    assert(!command->uniqueOffset(reinterpret_cast<const std::uint8_t*>(UINTPTR_MAX-255),256));
    std::printf("NATIVE_IDENTITY_SCALER_CANDIDATE: request_bits=%u coefficient_bits=%u retained_control_bits=%u PASS; input only,26controls unresolved, no command/GPU completion\n",
        requestBits, coefficientBits, controlBits);
    std::puts("NATIVE_IDENTITY_SCALER_PLAN:832control bits rejected,921600exact logical pixel mappings PASS; native input policy only, no GPU command/completion");
    std::puts("NATIVE_SCALER_COMMAND:2048bit identity controls, owned request, ordinary PM4 rejects PASS; no submission/completion");
    std::puts("NATIVE_SCALER_LOCATOR:8MiB bound, aligned positions/last-byte/duplicate rejection PASS; input inspection only");
}
