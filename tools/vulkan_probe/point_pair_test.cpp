// Actual translated raw pair OR our generic constant-copy pair, both with a
// synthetic viewport/attachment. This is not guest PM4 execution, EDRAM
// emulation, a game frame or a guest writeback.
#include "device.h"
#include "private_input.h"
#include "process_limits.h"
#include "native_gpu_word_updates.h"
#include "native_constant_point_inputs.h"
#include "native_gpu_word_readback.h"
#include "vulkan_constant_points.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace {
using namespace recomp::gpu::constant_points;
// Synthetic destinations in a native staging buffer, NOT guest physical RAM.
// Explicit byte packing works independently of host native word endianness.
void writeCompletionPhase(Resources& r, const recomp::gpu::NativeWordUpdatePlan& plan,
                          recomp::gpu::NativeWordPhase phase) {
    for (size_t i = 0; i < plan.count; ++i) {
        const auto& word = plan.words[i];
        if (word.phase == phase) {
            alignas(4) auto bytes = word.bigEndianBytes;
            if (r.corruptRecordedUpdate && phase == recomp::gpu::NativeWordPhase::AfterGraphics && word.byteOffset == 40)
                bytes[3] ^= 1; // Change GPU command data, NOT the validated plan.
            vkCmdUpdateBuffer(r.command,r.buffer,Bytes+word.byteOffset,bytes.size(),bytes.data());
        }
    }
}
bool draw(recomp::gpu::ProbeDevice& device, Resources& r, uint32_t writeMask, uint32_t draws,
          uint32_t expectedBlack, bool orderedCompletion = false, bool corruptCompletion = false,
          const recomp::gpu::NativeConstantPointInputs* inputs = nullptr, uint32_t expectedPointRgba = 0) {
    if (r.constantInputs != bool(inputs)) return false;
    using Phase = recomp::gpu::NativeWordPhase;
    const std::array<recomp::gpu::NativeWordRequest,9> requests{{
        {4,0x11223344,Phase::BeforeGraphics}, {8,0x55667788,Phase::BeforeGraphics},
        {12,0x99AABBCC,Phase::BeforeGraphics}, {20,0x10203040,Phase::BeforeGraphics},
        {24,0x50607080,Phase::BeforeGraphics}, {36,0x456789AB,Phase::AfterGraphics},
        {40,corruptCompletion ? 0xCDEF0122u : 0xCDEF0123u,Phase::AfterGraphics},
        {4,0xD00DC0DE,Phase::AfterGraphics}, {52,77,Phase::AfterGraphics}}};
    recomp::gpu::NativeWordUpdatePlan completionPlan;
    if (!recomp::gpu::planNativeWordUpdates(requests.data(),orderedCompletion ? requests.size() : 0,
        CompletionBytes,completionPlan)) return false;
    if (!pipeline(r, writeMask) || vkResetCommandPool(r.device, r.pool, 0) != VK_SUCCESS ||
        vkResetFences(r.device, 1, &r.fence) != VK_SUCCESS) return false;
    std::memset(r.mapped, 0x5A, TotalBytes);
    std::array<uint8_t, CompletionBytes> expectedCompletion{};
    expectedCompletion.fill(0x5A);
    if (orderedCompletion) expectedCompletion = {
        0x5A,0x5A,0x5A,0x5A, 0xD0,0x0D,0xC0,0xDE, 0x55,0x66,0x77,0x88, 0x99,0xAA,0xBB,0xCC,
        0x5A,0x5A,0x5A,0x5A, 0x10,0x20,0x30,0x40, 0x50,0x60,0x70,0x80, 0x5A,0x5A,0x5A,0x5A,
        0x5A,0x5A,0x5A,0x5A, 0x45,0x67,0x89,0xAB, 0xCD,0xEF,0x01,0x23, 0x5A,0x5A,0x5A,0x5A,
        0x5A,0x5A,0x5A,0x5A, 0x00,0x00,0x00,0x4D, 0x5A,0x5A,0x5A,0x5A, 0x5A,0x5A,0x5A,0x5A};
    VkCommandBufferBeginInfo begin{}; begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(r.command, &begin) != VK_SUCCESS) return false;
    VkMemoryBarrier before{}; before.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    before.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT; before.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(r.command, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &before, 0, nullptr, 0, nullptr);
    if (orderedCompletion) {
        // Five scratch-like writes precede graphics. These values/offsets are
        // synthetic canaries, not an embedded capture or title register model.
        writeCompletionPhase(r, completionPlan, Phase::BeforeGraphics);
        VkMemoryBarrier prefix{}; prefix.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        prefix.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; prefix.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(r.command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,
            0, 1, &prefix, 0, nullptr, 0, nullptr);
    }
    // Make every generic color distinguishable from its control background,
    // including all-white output. No rounding-dependent intermediate colors.
    const bool blackBackground = inputs && expectedPointRgba == 0xFFFFFFFF;
    VkClearValue clear{}; for (auto& component : clear.color.float32) component = blackBackground ? 0 : 1;
    VkRenderPassBeginInfo pass{}; pass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    pass.renderPass = r.pass; pass.framebuffer = r.framebuffer; pass.renderArea = {{0,0},{Side,Side}};
    pass.clearValueCount = 1; pass.pClearValues = &clear;
    vkCmdBeginRenderPass(r.command, &pass, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(r.command, VK_PIPELINE_BIND_POINT_GRAPHICS, r.pipeline);
    if (inputs) vkCmdPushConstants(r.command,r.layout,VK_SHADER_STAGE_VERTEX_BIT,0,sizeof(*inputs),inputs);
    for (uint32_t i = 0; i < draws; ++i) vkCmdDraw(r.command, 1, 1, 0, 0);
    vkCmdEndRenderPass(r.command);
    if (orderedCompletion) {
        // Include all earlier writes, including the prefix, before transfer
        // publication. This also orders the deliberate same-word overwrite.
        VkMemoryBarrier graphicsDone{}; graphicsDone.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        graphicsDone.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        graphicsDone.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(r.command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 1, &graphicsDone, 0, nullptr, 0, nullptr);
        writeCompletionPhase(r, completionPlan, Phase::AfterGraphics);
    }
    VkBufferImageCopy copy{}; copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; copy.imageExtent = {Side,Side,1};
    vkCmdCopyImageToBuffer(r.command, r.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, r.buffer, 1, &copy);
    VkMemoryBarrier after{}; after.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    after.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; after.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(r.command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &after, 0, nullptr, 0, nullptr);
    if (vkEndCommandBuffer(r.command) != VK_SUCCESS) return false;
    // Command recording must not publish even one host-visible byte or signal
    // the reset fence. Never inspect mapped GPU destinations while in flight.
    const auto* beforeSubmit = static_cast<const uint8_t*>(r.mapped);
    for (uint32_t i = 0; i < TotalBytes; ++i) if (beforeSubmit[i] != 0x5A) return false;
    if (vkGetFenceStatus(r.device, r.fence) != VK_NOT_READY) return false;
    VkSubmitInfo submit{}; submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1; submit.pCommandBuffers = &r.command;
    if (vkQueueSubmit(device.queue, 1, &submit, r.fence) != VK_SUCCESS ||
        vkWaitForFences(r.device, 1, &r.fence, VK_TRUE, 5'000'000'000ULL) != VK_SUCCESS) {
        std::fflush(stdout); std::_Exit(4); // No in-flight resource destruction or success claim.
    }
    uint32_t black = 0, white = 0, pointPixels = 0, backgroundPixels = 0;
    const auto* pixels = static_cast<const uint32_t*>(r.mapped);
    for (uint32_t i = 0; i < Pixels; ++i) {
        if (pixels[i] == 0) ++black;
        else if (pixels[i] == 0xFFFFFFFF) ++white;
        if (pixels[i] == expectedPointRgba) ++pointPixels;
        if (pixels[i] == (blackBackground ? 0u : 0xFFFFFFFFu)) ++backgroundPixels;
    }
    std::printf("POINT_PAIR_CASE mask=%u draws=%u black=%u white=%u fence=1\n", writeMask, draws, black, white);
    const auto* completion = static_cast<const uint8_t*>(r.mapped) + Bytes;
    const bool completionMatches = std::memcmp(completion, expectedCompletion.data(), CompletionBytes) == 0;
    recomp::gpu::AuditedNativeWords audited; recomp::gpu::NativeWordReadbackFailure auditError;
    const bool wordsAudited = recomp::gpu::auditNativeWordReadback(completionPlan,completion,CompletionBytes,audited,auditError);
    if (orderedCompletion) std::printf("NATIVE_WORD_READBACK_AUDIT passed=%u destinations=%zu caller_fence_completed=1 guest_write_authority=0\n",
        unsigned(wordsAudited),audited.count);
    if (orderedCompletion) std::printf("POINT_COMPLETION_CASE be_updates=9 checked_bytes=64 ordered_overwrite=%u before_submit_unchanged=1 fence=1 matched=%u guest_memory_written=0\n",
        unsigned(std::memcmp(completion + 4, expectedCompletion.data() + 4, 4) == 0), unsigned(completionMatches));
    if (inputs) {
        std::printf("GENERIC_CONSTANT_CASE rgba=%08X point_pixels=%u background_pixels=%u fence=1\n",expectedPointRgba,pointPixels,backgroundPixels);
        return pointPixels == expectedBlack && pointPixels + backgroundPixels == Pixels && completionMatches && wordsAudited;
    }
    return black == expectedBlack && white + black == Pixels && completionMatches && wordsAudited;
}
}
int main(int argc, char** argv) {
    const bool corruptCompletion = argc == 4 && std::strcmp(argv[3], "--corrupt-completion-control") == 0;
    const bool corruptRecordedUpdate = argc == 4 && std::strcmp(argv[3], "--corrupt-recorded-update") == 0;
    const bool corruptConstants = argc == 4 && std::strcmp(argv[3], "--generic-corrupt-constants") == 0;
    const bool genericConstants = corruptConstants || (argc == 4 && std::strcmp(argv[3], "--generic-constants") == 0);
    const bool genericRawPair = argc == 6 && std::strcmp(argv[3], "--generic-raw-pair") == 0;
    if ((argc != 3 && !corruptCompletion && !corruptRecordedUpdate && !genericConstants && !genericRawPair) || !recomp::gpu::initializeProbeProcess()) return 2;
    std::array<std::vector<uint32_t>, 2> code;
    for (size_t i = 0; i < code.size(); ++i) {
        std::vector<uint8_t> bytes;
        if (!recomp::gpu::readInput(argv[i + 1], bytes, 65536) || bytes.size() < 20 || bytes.size() % 4) return 2;
        code[i].resize(bytes.size() / 4); std::memcpy(code[i].data(), bytes.data(), bytes.size());
        if (code[i][0] != 0x07230203) return 2;
    }
    recomp::gpu::NativeConstantPointInputs rawInputs;
    if (genericRawPair) {
        std::array<recomp::gpu::CopyShaderProof,2> proofs;
        for (unsigned i = 0; i < 2; ++i) {
            std::vector<uint8_t> raw; recomp::gpu::CopyShaderError error;
            if (!recomp::gpu::readInput(argv[4+i],raw,4096) ||
                !recomp::gpu::analyzeCopyShader(raw.data(),raw.size(),static_cast<recomp::gpu::CopyShaderStage>(i),proofs[i],error)) return 2;
        }
        recomp::gpu::ConstantShaderPair pair;
        // Explicit diagnostic linkage/state profile, not a guest submission.
        // Prevalidate both raw inputs before creating even a native device.
        if (!recomp::gpu::linkConstantShaderPair(proofs[0],proofs[1],0x1000000E,0,pair) ||
            pair.position != std::array<uint8_t,4>{{0,0,0,1}} || pair.parameter != std::array<uint8_t,4>{{0,0,0,0}} ||
            !recomp::gpu::packNativeConstantPointInputs(pair,rawInputs)) return 2;
    }
    recomp::gpu::ProbeDevice device;
    if (!device.inspect() || !device.create({})) return 1;
    Resources resources(device.device);
    resources.corruptRecordedUpdate = corruptRecordedUpdate;
    if (genericRawPair) {
        if (!setup(device,resources,code,true) || !draw(device,resources,15,0,0,false,false,&rawInputs) ||
            !draw(device,resources,15,1,1,false,false,&rawInputs) || !draw(device,resources,0,24,0,false,false,&rawInputs) ||
            !draw(device,resources,0,24,0,true,false,&rawInputs)) return 1;
        std::puts("GENERIC_RAW_PAIR_V1 raw_programs_proved=2 cases=4 readback_pixels=1024 game_specific_spirv=0 guest_commands_executed=0 guest_writebacks=0");
        return 0;
    }
    if (genericConstants) {
        if (!setup(device,resources,code,true)) return 1;
        for (unsigned mask = 0; mask < 16; ++mask) {
            recomp::gpu::ConstantShaderPair pair{{0,0,0,1},{}};
            uint32_t expectedRgba = 0;
            for (unsigned component = 0; component < 4; ++component) {
                pair.parameter[component] = (mask >> component) & 1;
                if (mask & (1 << component)) expectedRgba |= 255u << (component*8);
            }
            recomp::gpu::NativeConstantPointInputs inputs;
            if (!recomp::gpu::packNativeConstantPointInputs(pair,inputs)) return 1;
            if (corruptConstants) inputs.parameter[0] = 1.0f; // Independent expected color is unchanged.
            if (!draw(device,resources,15,0,0,false,false,&inputs,expectedRgba) ||
                !draw(device,resources,15,1,1,false,false,&inputs,expectedRgba) ||
                !draw(device,resources,0,24,0,true,false,&inputs,expectedRgba)) return 1;
        }
        std::puts("GENERIC_CONSTANT_PAIR_V1 color_variants=16 cases=48 readback_pixels=12288 game_specific_spirv=0 guest_commands_executed=0 guest_writebacks=0");
        return 0;
    }
    if (!setup(device, resources, code) || !draw(device, resources, 15, 0, 0) ||
        !draw(device, resources, 15, 1, 1) || !draw(device, resources, 0, 24, 0) ||
        !draw(device, resources, 0, 24, 0, true, corruptCompletion)) return 1;
    std::puts("POINT_PAIR_V2 cases=4 readback_pixels=1024 guest_commands_executed=0 title_raster_state_replayed=0 guest_writebacks=0");
    return 0;
}
