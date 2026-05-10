#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/ext/quaternion_float.hpp>
#include <glm/ext/quaternion_trigonometric.hpp>
#include <glm/ext/vector_float2.hpp>
#include <glm/ext/vector_float3.hpp>
#include <glm/ext/vector_float4.hpp>
#include <glm/geometric.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/type_ptr.hpp>

namespace fxe::math {
  using vec2 = glm::vec2;
  using vec3 = glm::vec3;
  using vec4 = glm::vec4;
  using ivec2 = glm::ivec2;
  using ivec4 = glm::ivec4;
  using uvec2 = glm::uvec2;
  using dvec2 = glm::dvec2;
  using mat4x4 = glm::mat4;
  using quaternion = glm::quat;

  inline constexpr float flt_max = std::numeric_limits<float>::max();
  inline constexpr float pi = glm::pi<float>();

  [[nodiscard]] inline float to_rad(float deg) noexcept {
    return glm::radians(deg);
  }
  [[nodiscard]] inline float to_deg(float rad) noexcept {
    return glm::degrees(rad);
  }
  [[nodiscard]] inline float fsin(float x) noexcept {
    return std::sin(x);
  }
  [[nodiscard]] inline float fcos(float x) noexcept {
    return std::cos(x);
  }
  [[nodiscard]] inline std::pair<float, float> fsincos(float x) noexcept {
    return {std::sin(x), std::cos(x)};
  }
  [[nodiscard]] constexpr float rcp(float x) noexcept {
    return 1.0f / x;
  }
  [[nodiscard]] inline float fmod(float x, float y) noexcept {
    return std::fmod(x, y);
  }
  [[nodiscard]] constexpr float fclamp(float x, float lo, float hi) noexcept {
    return std::clamp(x, lo, hi);
  }
  [[nodiscard]] constexpr float fmin(float a, float b) noexcept {
    return std::min(a, b);
  }
  [[nodiscard]] constexpr float fmax(float a, float b) noexcept {
    return std::max(a, b);
  }
  [[nodiscard]] inline float ftrunc(float x) noexcept {
    return std::trunc(x);
  }

  [[nodiscard]] inline vec2 vec_min(vec2 a, vec2 b) noexcept {
    return glm::min(a, b);
  }
  [[nodiscard]] inline vec3 vec_min(vec3 a, vec3 b) noexcept {
    return glm::min(a, b);
  }
  [[nodiscard]] inline vec4 vec_min(vec4 a, vec4 b) noexcept {
    return glm::min(a, b);
  }
  [[nodiscard]] inline vec2 vec_max(vec2 a, vec2 b) noexcept {
    return glm::max(a, b);
  }
  [[nodiscard]] inline vec3 vec_max(vec3 a, vec3 b) noexcept {
    return glm::max(a, b);
  }
  [[nodiscard]] inline vec4 vec_max(vec4 a, vec4 b) noexcept {
    return glm::max(a, b);
  }
  [[nodiscard]] inline vec4 vec_round(vec4 v) noexcept {
    return glm::round(v);
  }

  using glm::cross;
  using glm::dot;
  using glm::length;
  using glm::normalize;

  [[nodiscard]] inline mat4x4 identity() noexcept {
    return mat4x4(1.0f);
  }

  [[nodiscard]] inline mat4x4 euler_to_matrix(vec3 radians) noexcept {
    mat4x4 m(1.0f);
    m = glm::rotate(m, radians.z, vec3{0, 0, 1});
    m = glm::rotate(m, radians.y, vec3{0, 1, 0});
    m = glm::rotate(m, radians.x, vec3{1, 0, 0});
    return m;
  }

  [[nodiscard]] inline mat4x4 direction_to_matrix(vec3 forward, vec3 up = {0, 1, 0}) noexcept {
    forward = glm::normalize(forward);
    vec3 right = glm::normalize(glm::cross(up, forward));
    vec3 real_up = glm::cross(forward, right);
    mat4x4 m(1.0f);
    m[0] = vec4(right, 0.0f);
    m[1] = vec4(real_up, 0.0f);
    m[2] = vec4(forward, 0.0f);
    return m;
  }

  [[nodiscard]] inline mat4x4 perspective_projection(float fov_y_radians, float aspect,
                                                     float z_near, float z_far) noexcept {
    return glm::perspectiveRH_ZO(fov_y_radians, aspect, z_near, z_far);
  }

  [[nodiscard]] inline mat4x4 make_transform(vec3 pos, vec3 scale, vec3 rot = {}) noexcept {
    mat4x4 m(1.0f);
    m = glm::translate(m, pos);
    m *= euler_to_matrix(rot);
    m = glm::scale(m, scale);
    return m;
  }

  [[nodiscard]] inline vec3 transform_point(const mat4x4& m, vec3 p) noexcept {
    vec4 r = m * vec4(p, 1.0f);
    return vec3(r) / (r.w == 0.0f ? 1.0f : r.w);
  }
} // namespace fxe::math
