// Goal step 4 feasibility probe (standalone, no game data): can the guest's 512 MB physical arena be Vulkan device memory
// that the CPU maps at fixed guest addresses (two aliases, like GuestAddressSpace::reserveWithPhysicalAcAlias) and the GPU
// reads in place?
//   1. lists memory types/heaps; picks HOST_VISIBLE|HOST_COHERENT(|HOST_CACHED, preferred) for an exportable buffer;
//   2. allocates SIZE_MB (default 512) with VkExportMemoryAllocateInfo(OPAQUE_FD), binds a STORAGE|TRANSFER|VERTEX|INDEX
//      buffer, exports the fd (vkGetMemoryFdKHR);
//   3. mmaps the fd MAP_SHARED|MAP_FIXED at two aliases of a fresh reservation (and, separately, vkMapMemory);
//   4. coherence: CPU pattern through alias A == alias B; GPU vkCmdCopyBuffer of CPU-written data to a readback buffer;
//      GPU vkCmdFillBuffer into the exported buffer read back by the CPU through alias B;
//   5. CPU speed on the fixed mapping vs anonymous memory: sequential read/write (memcpy) and a dependent random walk.
// Prints one PROBE line per result and ZERO_COPY_PROBE ... FEASIBLE/NOT_FEASIBLE. Read-only for the rest of the device.
// Build: NDK clang++ --target=aarch64-linux-android33 -std=c++20 -O2 zero_copy_probe.cpp -lvulkan -o zero_copy_probe
#include <vulkan/vulkan.h>
#include <sys/mman.h>
#include <unistd.h>
#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
double msSince(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }
#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { std::printf("PROBE fail %s = %d\n", #x, int(r_)); return 1; } } while (0)

double seqRead(const std::uint8_t* p, std::size_t n) { // GB/s
    const auto t = Clock::now(); std::uint64_t s = 0;
    for (std::size_t i = 0; i < n; i += 64) { std::uint64_t v; std::memcpy(&v, p + i, 8); s += v; }
    const double ms = msSince(t); if (s == 42) std::printf(" ");
    return double(n) / 1e6 / ms;
}
double seqWrite(std::uint8_t* p, std::size_t n) { const auto t = Clock::now(); std::memset(p, 0x5A, n); return double(n) / 1e6 / msSince(t); }
double copyRate(std::uint8_t* dst, const std::uint8_t* src, std::size_t n) { const auto t = Clock::now(); std::memcpy(dst, src, n); return double(n) / 1e6 / msSince(t); }
double randomWalkNs(std::uint8_t* p, std::size_t n, std::size_t steps) { // dependent loads over a random cycle of 64-byte lines
    const std::size_t lines = n / 64; std::vector<std::uint32_t> order(lines);
    for (std::size_t i = 0; i < lines; ++i) order[i] = std::uint32_t(i);
    std::mt19937 rng(4); std::shuffle(order.begin(), order.end(), rng);
    for (std::size_t i = 0; i < lines; ++i) { const std::uint64_t next = std::uint64_t(order[(i + 1) % lines]) * 64; std::memcpy(p + std::size_t(order[i]) * 64, &next, 8); }
    std::uint64_t at = std::uint64_t(order[0]) * 64; const auto t = Clock::now();
    for (std::size_t i = 0; i < steps; ++i) std::memcpy(&at, p + at, 8);
    const double ns = msSince(t) * 1e6 / double(steps); if (at == 1) std::printf(" ");
    return ns;
}
}

int main(int argc, char** argv) {
    const std::size_t sizeMb = argc > 1 ? std::strtoul(argv[1], nullptr, 10) : 512;
    const VkDeviceSize size = VkDeviceSize(sizeMb) << 20;
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.apiVersion = VK_API_VERSION_1_1; app.pApplicationName = "zero_copy_probe";
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ici.pApplicationInfo = &app;
    VkInstance instance; CHECK(vkCreateInstance(&ici, nullptr, &instance));
    std::uint32_t n = 0; vkEnumeratePhysicalDevices(instance, &n, nullptr); if (!n) { std::printf("PROBE no device\n"); return 1; }
    std::vector<VkPhysicalDevice> pds(n); vkEnumeratePhysicalDevices(instance, &n, pds.data()); VkPhysicalDevice pd = pds[0];
    VkPhysicalDeviceProperties props; vkGetPhysicalDeviceProperties(pd, &props);
    std::printf("PROBE device=%s api=%u.%u.%u driver=%u\n", props.deviceName, VK_VERSION_MAJOR(props.apiVersion), VK_VERSION_MINOR(props.apiVersion),
                VK_VERSION_PATCH(props.apiVersion), props.driverVersion);
    VkPhysicalDeviceMaintenance3Properties m3{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_3_PROPERTIES};
    VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2}; p2.pNext = &m3; vkGetPhysicalDeviceProperties2(pd, &p2);
    std::printf("PROBE maxMemoryAllocationSize=%llu MB maxStorageBufferRange=%u MB\n", (unsigned long long)(m3.maxMemoryAllocationSize >> 20),
                props.limits.maxStorageBufferRange >> 20);
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    for (std::uint32_t h = 0; h < mp.memoryHeapCount; ++h)
        std::printf("PROBE heap %u size=%llu MB flags=%x\n", h, (unsigned long long)(mp.memoryHeaps[h].size >> 20), mp.memoryHeaps[h].flags);
    for (std::uint32_t t = 0; t < mp.memoryTypeCount; ++t)
        std::printf("PROBE type %u heap %u flags=%x%s%s%s%s\n", t, mp.memoryTypes[t].heapIndex, mp.memoryTypes[t].propertyFlags,
                    mp.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT ? " device_local" : "",
                    mp.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT ? " host_visible" : "",
                    mp.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT ? " coherent" : "",
                    mp.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT ? " cached" : "");
    std::uint32_t extCount = 0; vkEnumerateDeviceExtensionProperties(pd, nullptr, &extCount, nullptr);
    std::vector<VkExtensionProperties> exts(extCount); vkEnumerateDeviceExtensionProperties(pd, nullptr, &extCount, exts.data());
    bool fdExt = false, dmabufExt = false, hostExt = false, ahbExt = false;
    for (const auto& e : exts) {
        fdExt |= !std::strcmp(e.extensionName, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME);
        dmabufExt |= !std::strcmp(e.extensionName, "VK_EXT_external_memory_dma_buf");
        hostExt |= !std::strcmp(e.extensionName, "VK_EXT_external_memory_host");
        ahbExt |= !std::strcmp(e.extensionName, "VK_ANDROID_external_memory_android_hardware_buffer");
    }
    std::printf("PROBE ext external_memory_fd=%d dma_buf=%d external_memory_host=%d android_hardware_buffer=%d\n", fdExt, dmabufExt, hostExt, ahbExt);
    if (!fdExt) { std::printf("ZERO_COPY_PROBE NOT_FEASIBLE reason=no-external-memory-fd\n"); return 0; }
    const VkBufferUsageFlags usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                     VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    VkPhysicalDeviceExternalBufferInfo ebi{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO}; ebi.usage = usage;
    ebi.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    VkExternalBufferProperties ebp{VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES}; vkGetPhysicalDeviceExternalBufferProperties(pd, &ebi, &ebp);
    std::printf("PROBE opaque_fd buffer features=%x (export=%d import=%d dedicated=%d)\n", ebp.externalMemoryProperties.externalMemoryFeatures,
                !!(ebp.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT),
                !!(ebp.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT),
                !!(ebp.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT));
    // Device with the fd extension and one queue that supports transfers.
    std::uint32_t qfn = 0; vkGetPhysicalDeviceQueueFamilyProperties(pd, &qfn, nullptr);
    std::vector<VkQueueFamilyProperties> qf(qfn); vkGetPhysicalDeviceQueueFamilyProperties(pd, &qfn, qf.data());
    std::uint32_t family = 0; for (std::uint32_t i = 0; i < qfn; ++i) if (qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { family = i; break; }
    const float prio = 1.0f; VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}; qci.queueFamilyIndex = family; qci.queueCount = 1; qci.pQueuePriorities = &prio;
    const char* devExts[] = {VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME};
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci; dci.enabledExtensionCount = 1; dci.ppEnabledExtensionNames = devExts;
    VkDevice dev; CHECK(vkCreateDevice(pd, &dci, nullptr, &dev));
    VkQueue queue; vkGetDeviceQueue(dev, family, 0, &queue);
    auto getFd = reinterpret_cast<PFN_vkGetMemoryFdKHR>(vkGetDeviceProcAddr(dev, "vkGetMemoryFdKHR"));
    // Exportable buffer + memory.
    VkExternalMemoryBufferCreateInfo embci{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO}; embci.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; bci.pNext = &embci; bci.size = size; bci.usage = usage; bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer buf; CHECK(vkCreateBuffer(dev, &bci, nullptr, &buf));
    VkMemoryRequirements req; vkGetBufferMemoryRequirements(dev, buf, &req);
    int chosen = -1; bool cached = false;
    for (int pass = 0; pass < 2 && chosen < 0; ++pass)
        for (std::uint32_t t = 0; t < mp.memoryTypeCount; ++t) {
            const auto f = mp.memoryTypes[t].propertyFlags;
            const bool ok = (req.memoryTypeBits >> t & 1) && (f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) && (f & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) &&
                            (pass == 1 || (f & VK_MEMORY_PROPERTY_HOST_CACHED_BIT));
            if (ok) { chosen = int(t); cached = f & VK_MEMORY_PROPERTY_HOST_CACHED_BIT; break; }
        }
    std::printf("PROBE buffer size=%llu MB alignment=%llu memoryTypeBits=%x chosen_type=%d cached=%d\n", (unsigned long long)(req.size >> 20),
                (unsigned long long)req.alignment, req.memoryTypeBits, chosen, cached);
    if (chosen < 0) { std::printf("ZERO_COPY_PROBE NOT_FEASIBLE reason=no-host-visible-coherent-exportable-type\n"); return 0; }
    VkExportMemoryAllocateInfo emai{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO}; emai.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO}; dedicated.buffer = buf; emai.pNext = &dedicated;
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; mai.pNext = &emai; mai.allocationSize = req.size; mai.memoryTypeIndex = std::uint32_t(chosen);
    VkDeviceMemory mem; const auto ta = Clock::now(); CHECK(vkAllocateMemory(dev, &mai, nullptr, &mem));
    std::printf("PROBE allocate %.1f ms\n", msSince(ta));
    CHECK(vkBindBufferMemory(dev, buf, mem, 0));
    VkMemoryGetFdInfoKHR gfi{VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR}; gfi.memory = mem; gfi.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    int fd = -1; CHECK(getFd(dev, &gfi, &fd));
    char link[256]{}; char proc[64]; std::snprintf(proc, sizeof proc, "/proc/self/fd/%d", fd);
    const auto ll = readlink(proc, link, sizeof link - 1); std::printf("PROBE exported fd=%d -> %s\n", fd, ll > 0 ? link : "?");
    // Two fixed aliases in a fresh reservation (the arena's 0xA0000000 / 0xC0000000 pattern).
    const std::size_t span = std::size_t(size);
    void* reserve = mmap(nullptr, span * 2 + (1u << 20), PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    auto* a = static_cast<std::uint8_t*>(reserve); auto* b = a + span;
    void* ma = mmap(a, span, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0);
    const int ea = ma == MAP_FAILED ? errno : 0;
    void* mb = mmap(b, span, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0);
    const int eb = mb == MAP_FAILED ? errno : 0;
    std::printf("PROBE fixed mmap alias A=%s(%d) B=%s(%d)\n", ma == a ? "ok" : "FAIL", ea, mb == b ? "ok" : "FAIL", eb);
    void* vkmap = nullptr; const VkResult mr = vkMapMemory(dev, mem, 0, VK_WHOLE_SIZE, 0, &vkmap);
    std::printf("PROBE vkMapMemory=%d ptr=%p\n", int(mr), vkmap);
    const bool fixedOk = ma == a && mb == b;
    std::uint8_t* cpu = fixedOk ? a : static_cast<std::uint8_t*>(vkmap);
    std::uint8_t* alias = fixedOk ? b : static_cast<std::uint8_t*>(vkmap);
    if (!cpu) { std::printf("ZERO_COPY_PROBE NOT_FEASIBLE reason=no-cpu-mapping\n"); return 0; }
    // Coherence 1: alias A write, alias B (and the vkMapMemory view) read.
    for (std::size_t i = 0; i < span; i += 4096) { const std::uint32_t v = std::uint32_t(i * 2654435761u); std::memcpy(cpu + i, &v, 4); }
    std::size_t aliasBad = 0, vkBad = 0;
    for (std::size_t i = 0; i < span; i += 4096) {
        const std::uint32_t v = std::uint32_t(i * 2654435761u); std::uint32_t x, y;
        std::memcpy(&x, alias + i, 4); aliasBad += x != v;
        if (vkmap) { std::memcpy(&y, static_cast<std::uint8_t*>(vkmap) + i, 4); vkBad += y != v; }
    }
    std::printf("PROBE alias coherence mismatches=%zu vkmap mismatches=%zu\n", aliasBad, vkBad);
    // Readback buffer (host visible) for GPU copies.
    VkBufferCreateInfo rbi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; rbi.size = 64 << 20; rbi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VkBuffer rb; CHECK(vkCreateBuffer(dev, &rbi, nullptr, &rb)); VkMemoryRequirements rreq; vkGetBufferMemoryRequirements(dev, rb, &rreq);
    int rtype = -1; for (std::uint32_t t = 0; t < mp.memoryTypeCount; ++t) if ((rreq.memoryTypeBits >> t & 1) &&
        (mp.memoryTypes[t].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) == (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) { rtype = int(t); break; }
    VkMemoryAllocateInfo rmai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; rmai.allocationSize = rreq.size; rmai.memoryTypeIndex = std::uint32_t(rtype);
    VkDeviceMemory rmem; CHECK(vkAllocateMemory(dev, &rmai, nullptr, &rmem)); CHECK(vkBindBufferMemory(dev, rb, rmem, 0));
    void* rmap = nullptr; CHECK(vkMapMemory(dev, rmem, 0, VK_WHOLE_SIZE, 0, &rmap));
    VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; cpi.queueFamilyIndex = family; cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    VkCommandPool pool; CHECK(vkCreateCommandPool(dev, &cpi, nullptr, &pool));
    VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; cbai.commandPool = pool; cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbai.commandBufferCount = 1;
    VkCommandBuffer cb; CHECK(vkAllocateCommandBuffers(dev, &cbai, &cb));
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; VkFence fence; CHECK(vkCreateFence(dev, &fci, nullptr, &fence));
    const auto runOnce = [&](auto record) -> double {
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkResetCommandBuffer(cb, 0); vkBeginCommandBuffer(cb, &bi); record(); vkEndCommandBuffer(cb);
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO}; si.commandBufferCount = 1; si.pCommandBuffers = &cb;
        vkResetFences(dev, 1, &fence); const auto t = Clock::now(); vkQueueSubmit(queue, 1, &si, fence);
        vkWaitForFences(dev, 1, &fence, VK_TRUE, 5'000'000'000ull); return msSince(t);
    };
    // Coherence 2: CPU writes 64 MB at the top of the arena, GPU copies it to the readback buffer.
    const VkDeviceSize off = size - (64 << 20);
    std::mt19937 rng(7); for (VkDeviceSize i = 0; i < (64 << 20); i += 4) { const std::uint32_t v = rng(); std::memcpy(cpu + off + i, &v, 4); }
    const double copyMs = runOnce([&] { VkBufferCopy c{off, 0, 64 << 20}; vkCmdCopyBuffer(cb, buf, rb, 1, &c);
        VkMemoryBarrier mb2{VK_STRUCTURE_TYPE_MEMORY_BARRIER}; mb2.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; mb2.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb2, 0, nullptr, 0, nullptr); });
    const bool gpuSawCpu = std::memcmp(rmap, cpu + off, 64 << 20) == 0;
    std::printf("PROBE gpu reads cpu-written arena: %s (64 MB copy %.2f ms = %.1f GB/s)\n", gpuSawCpu ? "ok" : "MISMATCH", copyMs, 64.0 / 1024.0 / (copyMs / 1000.0));
    // Coherence 3: GPU fills 16 MB in the arena, CPU reads it through the other alias.
    const double fillMs = runOnce([&] { vkCmdFillBuffer(cb, buf, 1 << 20, 16 << 20, 0xC0FFEE11u);
        VkMemoryBarrier mb2{VK_STRUCTURE_TYPE_MEMORY_BARRIER}; mb2.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; mb2.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb2, 0, nullptr, 0, nullptr); });
    std::size_t fillBad = 0; for (std::size_t i = 1 << 20; i < (17u << 20); i += 4) { std::uint32_t v; std::memcpy(&v, alias + i, 4); fillBad += v != 0xC0FFEE11u; }
    std::printf("PROBE cpu reads gpu-filled arena through alias B: mismatches=%zu (16 MB fill %.2f ms)\n", fillBad, fillMs);
    // CPU speed: the fixed mapping vs ordinary anonymous memory (256 MB each; random walk over 64 MB).
    const std::size_t bench = std::min<std::size_t>(span / 2, 256u << 20);
    auto* anon = static_cast<std::uint8_t*>(mmap(nullptr, bench * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    std::memset(anon, 1, bench * 2);
    for (int rep = 0; rep < 2; ++rep) {
        std::printf("PROBE cpu speed rep %d: arena read %.1f GB/s write %.1f copy %.1f walk %.1f ns | anon read %.1f write %.1f copy %.1f walk %.1f ns\n", rep,
                    seqRead(cpu, bench), seqWrite(cpu, bench), copyRate(cpu, cpu + bench, bench), randomWalkNs(cpu, 64u << 20, 2'000'000),
                    seqRead(anon, bench), seqWrite(anon, bench), copyRate(anon, anon + bench, bench), randomWalkNs(anon, 64u << 20, 2'000'000));
    }
    const bool ok = fixedOk && !aliasBad && gpuSawCpu && !fillBad;
    std::printf("ZERO_COPY_PROBE size_mb=%zu fixed_aliases=%d cached=%d gpu_reads_cpu=%d cpu_reads_gpu=%d %s\n", sizeMb, fixedOk, cached, gpuSawCpu,
                fillBad == 0, ok ? "FEASIBLE" : "NOT_FEASIBLE");
    return 0;
}
