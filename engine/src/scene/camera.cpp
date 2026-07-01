#include "sq/scene/camera.hpp"

#include <glm/gtc/matrix_transform.hpp>

namespace sq::scene {

glm::mat4 Camera::view_projection(float aspect) const {
    // TODO（学習のため各自実装）:
    //   glm::mat4 proj = glm::perspective(fov_y_radians, aspect, near_plane, far_plane);
    //   proj[1][1] *= -1;  // VulkanはY下向き。上下反転を補正
    //   glm::mat4 view = glm::lookAt(position, target, up);
    //   return proj * view;
    (void)aspect;
    return glm::mat4(1.0f);
}

}  // namespace sq::scene
