#include "sq/graphics/debug_messenger.hpp"

#include <stdexcept>
#include <spdlog/spdlog.h>

namespace sq::graphics {

DebugMessenger::DebugMessenger(VkInstance instance, bool enabled)
    : instance_(instance), enabled_(enabled) {
    if (enabled_) {
        auto create_info = make_create_info();
        auto func = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance_, "vkCreateDebugUtilsMessengerEXT"));
        if (func) {
            if (func(instance_, &create_info, nullptr, &messenger_) != VK_SUCCESS) {
                throw std::runtime_error("デバッグメッセンジャーの作成に失敗しました！");
            }
        } else {
            throw std::runtime_error("デバッグメッセンジャーの作成に失敗しました！");
        }
    }
}

DebugMessenger::~DebugMessenger() {
    if (enabled_) {
        auto func = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance_, "vkDestroyDebugUtilsMessengerEXT"));
        if (func) {
            func(instance_, messenger_, nullptr);
        }
    }
}

VkDebugUtilsMessengerCreateInfoEXT DebugMessenger::make_create_info() {
    VkDebugUtilsMessengerCreateInfoEXT create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    create_info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    create_info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    create_info.pfnUserCallback = reinterpret_cast<PFN_vkDebugUtilsMessengerCallbackEXT>(debug_callback);

    return create_info;
}

VkBool32 DebugMessenger::debug_callback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                         VkDebugUtilsMessageTypeFlagsEXT type,
                                         const VkDebugUtilsMessengerCallbackDataEXT* callback_data,
                                         const void* user_data) {
    switch (severity) {
    case VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT:
        spdlog::debug(callback_data->pMessage);
        break;
    case VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT:
        spdlog::info(callback_data->pMessage);
        break;
    case VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT:
        spdlog::warn(callback_data->pMessage);
        break;
    case VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT:
        spdlog::error(callback_data->pMessage);
        break;
    default:
        break;
    }

    (void)severity;
    (void)type;
    (void)callback_data;
    (void)user_data;
    return VK_FALSE;
}

}  // namespace sq::graphics
