#include "shark/render_components.hpp"

#include <cmath>
#include <numbers>

namespace shark {

Result<Camera> validate_camera(Camera camera) {
    const auto fail = [](const char* message) -> Result<Camera> {
        return Error{ErrorCode::invalid_argument, message, {.operation = "camera.validate"}};
    };
    if (camera.projection != CameraProjection::perspective &&
        camera.projection != CameraProjection::orthographic) {
        return fail("Unsupported camera projection");
    }
    if (!std::isfinite(camera.vertical_fov_radians) || camera.vertical_fov_radians <= 0.0 ||
        camera.vertical_fov_radians >= std::numbers::pi ||
        !std::isfinite(camera.orthographic_height) || camera.orthographic_height <= 0.0) {
        return fail("Camera field of view must be within (0, pi) and height finite and positive");
    }
    if (!std::isfinite(camera.near_clip) || !std::isfinite(camera.far_clip) ||
        camera.near_clip < 0.0 || camera.far_clip <= camera.near_clip ||
        (camera.projection == CameraProjection::perspective && camera.near_clip == 0.0)) {
        return fail(
            "Camera requires finite far > near, near > 0 for perspective or >= 0 for orthographic");
    }
    return camera;
}

Result<MeshRenderer> validate_mesh_renderer(MeshRenderer renderer) {
    // All representable fields are valid locally. AssetId enforces UUIDv4 at
    // construction; mesh_index is checked against actual prepared content later.
    return renderer;
}

} // namespace shark
