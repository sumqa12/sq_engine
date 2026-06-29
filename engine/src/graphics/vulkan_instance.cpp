#include "sq/graphics/vulkan_instance.hpp"

namespace sq::graphics {

const char* VulkanInstance::kValidationLayerName = "VK_LAYER_KHRONOS_validation";

VulkanInstance::VulkanInstance(const std::string& app_name,
                                const char** required_extensions) {
    // ビルドがDebugの場合、バリデーションレイヤーのサポートを確認する。
#if !defined(NDEBUG)
    validation_enabled_ = supports_validation_layer();
#else
    validation_enabled_ = false;
#endif

    // VkApplicationInfo を設定する。
    VkApplicationInfo app_info{};
    app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app_info.pApplicationName = app_name.c_str();
    app_info.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    app_info.pEngineName = "SQ Engine";
    app_info.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    app_info.apiVersion = VK_API_VERSION_1_3;

    // validationがenabledの場合、required_extensionsにVK_EXT_debug_utilsを追加する。
    std::vector extensions(required_extensions, required_extensions + 2);
    if (validation_enabled_) {
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        required_extensions = extensions.data();
    }

    // VkInstanceCreateInfo を設定する。
    VkInstanceCreateInfo create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    create_info.pApplicationInfo = &app_info;
    create_info.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    create_info.ppEnabledExtensionNames = extensions.data();

    if (validation_enabled_) {
        create_info.enabledLayerCount = 1;
        create_info.ppEnabledLayerNames = &kValidationLayerName;
    } else {
        create_info.enabledLayerCount = 0;
        create_info.ppEnabledLayerNames = nullptr;
    }

    // Vulkanインスタンスを作成する。
    vkCreateInstance(&create_info, nullptr, &instance_);

    (void)app_name;
    (void)required_extensions;
}

// VulkanInstance のデストラクタ。Vulkanインスタンスを破棄する。
VulkanInstance::~VulkanInstance() {
    if (instance_ != VK_NULL_HANDLE) vkDestroyInstance(instance_, nullptr);
}

// VulkanInstance のハンドルを返す。
VkInstance VulkanInstance::handle() const {
    return instance_;
}

// VulkanInstance がバリデーションレイヤーを有効にしているかどうかを返す。
bool VulkanInstance::validation_enabled() const {
    return validation_enabled_;
}

// VulkanInstance がバリデーションレイヤーをサポートしているかどうかを確認する。
bool VulkanInstance::supports_validation_layer() {
    uint32_t layer_count = 0;
    vkEnumerateInstanceLayerProperties(&layer_count, nullptr);
    std::vector<VkLayerProperties> available_layers(layer_count);
    vkEnumerateInstanceLayerProperties(&layer_count, available_layers.data());

    for (const auto& layer : available_layers) {
        if (strcmp(layer.layerName, kValidationLayerName) == 0) {
            return true;
        }
    }
    return false;
}

}  // namespace sq::graphics
