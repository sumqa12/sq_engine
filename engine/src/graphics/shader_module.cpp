#include "sq/graphics/shader_module.hpp"

#include <filesystem>
#include <fstream>
#include <format>
#include <vector>

namespace sq::graphics {

VkShaderModule load_shader_module(VkDevice device, const std::string& spv_path) {
    // シェーダーファイルを読み込んで、シェーダーモジュールを作成する
    uintmax_t spv_file_size = 0;
    try {
        std::filesystem::path p = spv_path;
        spv_file_size = std::filesystem::file_size(p);
        printf("File size: %ju bytes\n", spv_file_size);
    } catch (const std::filesystem::filesystem_error& e) {
        printf("File size: %s bytes\n", e.what());
        printf("Path: %ls\n", e.path1().c_str());
        throw std::runtime_error(std::string(e.what()));
    }

    if (spv_file_size == 0) {
        throw std::runtime_error(std::format("load_shader_module : ファイルサイズが0です。path: {}", spv_path));
    }

    std::ifstream spv_file(spv_path, std::ios::binary);

    std::vector<char> spv(spv_file_size);
    spv_file.read(spv.data(), static_cast<std::streamsize>(spv_file_size));

    VkShaderModuleCreateInfo create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    create_info.codeSize = spv_file_size;
    create_info.pCode = reinterpret_cast<const uint32_t*>(spv.data());

    VkShaderModule shader_module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device, &create_info, nullptr, &shader_module) != VK_SUCCESS) {
        throw std::runtime_error("load_shader_module : シェーダーモジュールの作成に失敗しました。");
    }

    return shader_module;
}

}  // namespace sq::graphics
