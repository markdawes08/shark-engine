#pragma once

// This is Shark's only GLM include point. Every consumer shares these settings
// through SharkMath; reject prior setup rather than accept translation-unit drift.
#if defined(GLM_VERSION) || defined(GLM_SETUP_INCLUDED)
#error "Include shark/math.hpp before any GLM header"
#endif

#if defined(GLM_FORCE_LEFT_HANDED) || defined(GLM_FORCE_ALIGNED) ||                                \
    defined(GLM_FORCE_ALIGNED_GENTYPES) || defined(GLM_FORCE_DEFAULT_ALIGNED_GENTYPES) ||          \
    defined(GLM_FORCE_QUAT_DATA_WXYZ) || defined(GLM_FORCE_QUAT_DATA_XYZW) ||                      \
    defined(GLM_FORCE_SIZE_T_LENGTH) || defined(GLM_FORCE_SINGLE_ONLY) ||                          \
    defined(GLM_FORCE_SWIZZLE) || defined(GLM_FORCE_XYZW_ONLY) || defined(GLM_FORCE_INTRINSICS) || \
    defined(GLM_FORCE_SSE) || defined(GLM_FORCE_SSE2) || defined(GLM_FORCE_SSE3) ||                \
    defined(GLM_FORCE_SSSE3) || defined(GLM_FORCE_SSE41) || defined(GLM_FORCE_SSE42) ||            \
    defined(GLM_FORCE_AVX) || defined(GLM_FORCE_AVX2) || defined(GLM_FORCE_NEON)
#error "GLM configuration conflicts with Shark's packed right-handed scalar math contract"
#endif

#ifndef GLM_FORCE_RADIANS
#define GLM_FORCE_RADIANS
#endif
#ifndef GLM_FORCE_DEPTH_ZERO_TO_ONE
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#endif
#ifndef GLM_FORCE_EXPLICIT_CTOR
#define GLM_FORCE_EXPLICIT_CTOR
#endif
#ifndef GLM_FORCE_CTOR_INIT
#define GLM_FORCE_CTOR_INIT
#endif
#ifndef GLM_FORCE_PURE
#define GLM_FORCE_PURE
#endif

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

namespace shark::math {

using Vec3 = glm::dvec3;
using Quat = glm::dquat;
using Mat4 = glm::dmat4;

static_assert(sizeof(Vec3) == 3 * sizeof(double) && alignof(Vec3) == alignof(double));
static_assert(sizeof(Quat) == 4 * sizeof(double) && alignof(Quat) == alignof(double));
static_assert(sizeof(Mat4) == 16 * sizeof(double) && alignof(Mat4) == alignof(double));

} // namespace shark::math
