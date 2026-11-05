#include "xenos_copy_shader.h"
#include "private_input.h"
#include <iostream>
#include <string_view>

// Bounded diagnostic. Prints semantic proof metadata, never raw microcode.
// --stdin is for differential tests; file mode rejects symlinks/nonregular files.
int main(int argc, char** argv) {
    using namespace recomp::gpu;
    if (argc != 3) return 2;
    CopyShaderStage stage;
    if (std::string_view(argv[1]) == "vertex") stage = CopyShaderStage::Vertex;
    else if (std::string_view(argv[1]) == "pixel") stage = CopyShaderStage::Pixel;
    else return 2;
    std::vector<std::uint8_t> bytes;
    if (std::string_view(argv[2]) == "--stdin") {
        std::array<char,4097> buffer{};
        std::cin.read(buffer.data(),buffer.size());
        if (std::cin.bad()) return 2;
        bytes.assign(buffer.begin(),buffer.begin()+std::cin.gcount());
    } else if (!readInput(argv[2],bytes,4096)) return 2;
    CopyShaderProof proof; CopyShaderError error;
    if (!analyzeCopyShader(bytes.data(),bytes.size(),stage,proof,error)) {
        std::cout << "{\"accepted\":false,\"error\":" << static_cast<unsigned>(error.failure) << "}\n";
        return 1;
    }
    std::cout << "{\"accepted\":true,\"control_count\":" << proof.controlCount << ",\"executed\":[";
    for (unsigned i = 0; i < proof.executedCount; ++i) { if (i) std::cout << ','; std::cout << proof.executedAddresses[i]; }
    std::cout << "],\"exports\":{";
    bool first = true;
    for (unsigned i = 0; i < 64; ++i) if (proof.exported[i]) {
        if (!first) std::cout << ',';
        first = false; std::cout << '"' << i << "\":[";
        for (unsigned c = 0; c < 4; ++c) { if (c) std::cout << ','; std::cout << proof.exports[i][c]; }
        std::cout << ']';
    }
    std::cout << "},\"initial_components\":["; first = true;
    for (unsigned i = 0; i < 256; ++i) if (proof.initialComponentsRead[i]) {
        if (!first) std::cout << ',';
        first = false; std::cout << i;
    }
    std::cout << "],\"gpu_executed\":false}\n";
}
