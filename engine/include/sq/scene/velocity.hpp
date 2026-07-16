#pragma once

namespace sq::scene {
    struct Velocity {
        float vx = 0.0f;
        float vy = 0.0f;
        float vz = 0.0f;

        glm::vec3 operator+(const glm::vec3& vector) const {
            return {vx + vector.x, vy + vector.y, vz + vector.z};
        }

        glm::vec3 operator-(const glm::vec3& vector) const {
            return {vx - vector.x, vy - vector.y, vz - vector.z};
        }

        glm::vec3 operator*(const glm::vec3& vector) const {
            return {vx * vector.x, vy * vector.y, vz * vector.z};
        }

        glm::vec3 operator/(const glm::vec3& vector) const {
            return {vx / vector.x, vy / vector.y, vz / vector.z};
        }

        glm::vec3 operator*(const float scale) const {
            return {vx * scale, vy * scale, vz * scale};
        }

        Velocity& operator=(const glm::vec3& vector) {
            vx = vector.x;
            vy = vector.y;
            vz = vector.z;
            return *this;
        }
    };
}  // namespace sq::scene