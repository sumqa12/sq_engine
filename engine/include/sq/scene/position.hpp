#pragma once
#include "velocity.hpp"

namespace sq::scene {
    struct Position {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;

        glm::vec3 operator+(const glm::vec3& vector) const {
            return {x + vector.x, y + vector.y, z + vector.z};
        }

        glm::vec3 operator-(const glm::vec3& vector) const {
            return {x - vector.x, y - vector.y, z - vector.z};
        }

        Position& operator=(const glm::vec3& vector) {
            x = vector.x;
            y = vector.y;
            z = vector.z;
            return *this;
        }

        Position &operator+=(const Velocity & vel) {
            x += vel.vx;
            y += vel.vy;
            z += vel.vz;
            return *this;
        }

        Position &operator+=(const glm::vec3& vector) {
            x += vector.x;
            y += vector.y;
            z += vector.z;
            return *this;
        }
    };
}  // namespace sq::scene