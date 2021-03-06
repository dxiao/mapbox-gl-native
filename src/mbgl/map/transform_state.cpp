#include <mbgl/map/transform_state.hpp>
#include <mbgl/tile/tile_id.hpp>
#include <mbgl/util/constants.hpp>
#include <mbgl/util/interpolate.hpp>
#include <mbgl/math/log2.hpp>
#include <mbgl/math/clamp.hpp>

namespace mbgl {

TransformState::TransformState(ConstrainMode constrainMode_, ViewportMode viewportMode_)
    : constrainMode(constrainMode_)
    , viewportMode(viewportMode_)
{
}

#pragma mark - Matrix

void TransformState::matrixFor(mat4& matrix, const UnwrappedTileID& tileID) const {
    const uint64_t tileScale = 1ull << tileID.canonical.z;
    const double s = Projection::worldSize(scale) / tileScale;

    matrix::identity(matrix);
    matrix::translate(matrix, matrix,
                      int64_t(tileID.canonical.x + tileID.wrap * tileScale) * s,
                      int64_t(tileID.canonical.y) * s, 0);
    matrix::scale(matrix, matrix, s / util::EXTENT, s / util::EXTENT, 1);
}

void TransformState::getProjMatrix(mat4& projMatrix) const {

     // Find the distance from the center point [width/2, height/2] to the
    // center top point [width/2, 0] in Z units, using the law of sines.
    // 1 Z unit is equivalent to 1 horizontal px at the center of the map
    // (the distance between[width/2, height/2] and [width/2 + 1, height/2])
    const double halfFov = getFieldOfView() / 2.0;
    const double groundAngle = M_PI / 2.0 + getPitch();
    const double topHalfSurfaceDistance = std::sin(halfFov) * getCameraToCenterDistance() / std::sin(M_PI - groundAngle - halfFov);


    // Calculate z distance of the farthest fragment that should be rendered.
    const double furthestDistance = std::cos(M_PI / 2 - getPitch()) * topHalfSurfaceDistance + getCameraToCenterDistance();
    // Add a bit extra to avoid precision problems when a fragment's distance is exactly `furthestDistance`
    const double farZ = furthestDistance * 1.01;

    matrix::perspective(projMatrix, getFieldOfView(), double(size.width) / size.height, 1, farZ);

    const bool flippedY = viewportMode == ViewportMode::FlippedY;
    matrix::scale(projMatrix, projMatrix, 1, flippedY ? 1 : -1, 1);

    matrix::translate(projMatrix, projMatrix, 0, 0, -getCameraToCenterDistance());

    using NO = NorthOrientation;
    switch (getNorthOrientation()) {
        case NO::Rightwards: matrix::rotate_y(projMatrix, projMatrix, getPitch()); break;
        case NO::Downwards: matrix::rotate_x(projMatrix, projMatrix, -getPitch()); break;
        case NO::Leftwards: matrix::rotate_y(projMatrix, projMatrix, -getPitch()); break;
        default: matrix::rotate_x(projMatrix, projMatrix, getPitch()); break;
    }

    matrix::rotate_z(projMatrix, projMatrix, getAngle() + getNorthOrientationAngle());

    matrix::translate(projMatrix, projMatrix, pixel_x() - size.width / 2.0f,
                      pixel_y() - size.height / 2.0f, 0);
}

#pragma mark - Dimensions

Size TransformState::getSize() const {
    return size;
}

#pragma mark - North Orientation

NorthOrientation TransformState::getNorthOrientation() const {
    return orientation;
}

double TransformState::getNorthOrientationAngle() const {
    double angleOrientation = 0;
    if (orientation == NorthOrientation::Rightwards) {
        angleOrientation += M_PI / 2.0f;
    } else if (orientation == NorthOrientation::Downwards) {
        angleOrientation += M_PI;
    } else if (orientation == NorthOrientation::Leftwards) {
        angleOrientation -= M_PI / 2.0f;
    }
    return angleOrientation;
}

#pragma mark - Constrain mode

ConstrainMode TransformState::getConstrainMode() const {
    return constrainMode;
}

#pragma mark - ViewportMode

ViewportMode TransformState::getViewportMode() const {
    return viewportMode;
}

#pragma mark - Position

LatLng TransformState::getLatLng(LatLng::WrapMode wrapMode) const {
    return {
        util::RAD2DEG * (2 * std::atan(std::exp(y / Cc)) - 0.5 * M_PI),
        -x / Bc,
        wrapMode
    };
}

double TransformState::pixel_x() const {
    const double center = (size.width - Projection::worldSize(scale)) / 2;
    return center + x;
}

double TransformState::pixel_y() const {
    const double center = (size.height - Projection::worldSize(scale)) / 2;
    return center + y;
}

#pragma mark - Zoom

double TransformState::getZoom() const {
    return scaleZoom(scale);
}

int32_t TransformState::getIntegerZoom() const {
    return getZoom();
}

double TransformState::getZoomFraction() const {
    return getZoom() - getIntegerZoom();
}

double TransformState::getScale() const {
    return scale;
}

void TransformState::setMinZoom(const double minZoom) {
    if (minZoom <= getMaxZoom()) {
        min_scale = zoomScale(util::clamp(minZoom, util::MIN_ZOOM, util::MAX_ZOOM));
    }
}

double TransformState::getMinZoom() const {
    double test_scale = min_scale;
    double unused_x = x;
    double unused_y = y;
    constrain(test_scale, unused_x, unused_y);

    return scaleZoom(test_scale);
}

void TransformState::setMaxZoom(const double maxZoom) {
    if (maxZoom >= getMinZoom()) {
        max_scale = zoomScale(util::clamp(maxZoom, util::MIN_ZOOM, util::MAX_ZOOM));
    }
}

double TransformState::getMaxZoom() const {
    return scaleZoom(max_scale);
}


#pragma mark - Rotation

float TransformState::getAngle() const {
    return angle;
}

float TransformState::getFieldOfView() const {
    return fov;
}

float TransformState::getCameraToCenterDistance() const {
    return 0.5 * size.height / std::tan(fov / 2.0);
}

float TransformState::getPitch() const {
    return pitch;
}


#pragma mark - State

bool TransformState::isChanging() const {
    return rotating || scaling || panning || gestureInProgress;
}

bool TransformState::isRotating() const {
    return rotating;
}

bool TransformState::isScaling() const {
    return scaling;
}

bool TransformState::isPanning() const {
    return panning;
}

bool TransformState::isGestureInProgress() const {
    return gestureInProgress;
}


#pragma mark - Projection

double TransformState::zoomScale(double zoom) const {
    return std::pow(2.0, zoom);
}

double TransformState::scaleZoom(double s) const {
    return util::log2(s);
}

ScreenCoordinate TransformState::latLngToScreenCoordinate(const LatLng& latLng) const {
    if (!size) {
        return {};
    }

    mat4 mat = coordinatePointMatrix(getZoom());
    vec4 p;
    Point<double> pt = Projection::project(latLng, scale) / double(util::tileSize);
    vec4 c = {{ pt.x, pt.y, 0, 1 }};
    matrix::transformMat4(p, c, mat);
    return { p[0] / p[3], size.height - p[1] / p[3] };
}

LatLng TransformState::screenCoordinateToLatLng(const ScreenCoordinate& point, LatLng::WrapMode wrapMode) const {
    if (!size) {
        return {};
    }

    float targetZ = 0;
    mat4 mat = coordinatePointMatrix(getZoom());

    mat4 inverted;
    bool err = matrix::invert(inverted, mat);

    if (err) throw std::runtime_error("failed to invert coordinatePointMatrix");

    double flippedY = size.height - point.y;

    // since we don't know the correct projected z value for the point,
    // unproject two points to get a line and then find the point on that
    // line with z=0

    vec4 coord0;
    vec4 coord1;
    vec4 point0 = {{ point.x, flippedY, 0, 1 }};
    vec4 point1 = {{ point.x, flippedY, 1, 1 }};
    matrix::transformMat4(coord0, point0, inverted);
    matrix::transformMat4(coord1, point1, inverted);

    double w0 = coord0[3];
    double w1 = coord1[3];

    Point<double> p0 = Point<double>(coord0[0], coord0[1]) / w0;
    Point<double> p1 = Point<double>(coord1[0], coord1[1]) / w1;

    double z0 = coord0[2] / w0;
    double z1 = coord1[2] / w1;
    double t = z0 == z1 ? 0 : (targetZ - z0) / (z1 - z0);

    return Projection::unproject(util::interpolate(p0, p1, t), scale / util::tileSize, wrapMode);
}

mat4 TransformState::coordinatePointMatrix(double z) const {
    mat4 proj;
    getProjMatrix(proj);
    float s = Projection::worldSize(scale) / std::pow(2, z);
    matrix::scale(proj, proj, s, s, 1);
    matrix::multiply(proj, getPixelMatrix(), proj);
    return proj;
}

mat4 TransformState::getPixelMatrix() const {
    mat4 m;
    matrix::identity(m);
    matrix::scale(m, m,
                  static_cast<double>(size.width) / 2, -static_cast<double>(size.height) / 2, 1);
    matrix::translate(m, m, 1, -1, 0);
    return m;
}


#pragma mark - (private helper functions)

bool TransformState::rotatedNorth() const {
    using NO = NorthOrientation;
    return (orientation == NO::Leftwards || orientation == NO::Rightwards);
}

void TransformState::constrain(double& scale_, double& x_, double& y_) const {
    // Constrain minimum scale to avoid zooming out far enough to show off-world areas.
    scale_ = util::max(scale_,
                       static_cast<double>(rotatedNorth() ? size.height : size.width) / util::tileSize,
                       static_cast<double>(rotatedNorth() ? size.width : size.height) / util::tileSize);

    // Constrain min/max pan to avoid showing off-world areas.
    if (constrainMode == ConstrainMode::WidthAndHeight) {
        double max_x = (scale_ * util::tileSize - (rotatedNorth() ? size.height : size.width)) / 2;
        x_ = std::max(-max_x, std::min(x_, max_x));
    }

    if (constrainMode != ConstrainMode::None) {
        double max_y = (scale_ * util::tileSize - (rotatedNorth() ? size.width : size.height)) / 2;
        y_ = std::max(-max_y, std::min(y_, max_y));
    }
}

void TransformState::moveLatLng(const LatLng& latLng, const ScreenCoordinate& anchor) {
    auto centerCoord = Projection::project(getLatLng(LatLng::Unwrapped), scale);
    auto latLngCoord = Projection::project(latLng, scale);
    auto anchorCoord = Projection::project(screenCoordinateToLatLng(anchor), scale);
    setLatLngZoom(Projection::unproject(centerCoord + latLngCoord - anchorCoord, scale), getZoom());
}

void TransformState::setLatLngZoom(const LatLng &latLng, double zoom) {
    double newScale = zoomScale(zoom);
    const double newWorldSize = newScale * util::tileSize;
    Bc = newWorldSize / util::DEGREES_MAX;
    Cc = newWorldSize / util::M2PI;

    const double m = 1 - 1e-15;
    const double f = util::clamp(std::sin(util::DEG2RAD * latLng.latitude()), -m, m);

    ScreenCoordinate point = {
        -latLng.longitude() * Bc,
        0.5 * Cc * std::log((1 + f) / (1 - f)),
    };
    setScalePoint(newScale, point);
}

void TransformState::setScalePoint(const double newScale, const ScreenCoordinate &point) {
    double constrainedScale = newScale;
    ScreenCoordinate constrainedPoint = point;
    constrain(constrainedScale, constrainedPoint.x, constrainedPoint.y);

    scale = constrainedScale;
    x = constrainedPoint.x;
    y = constrainedPoint.y;
    Bc = Projection::worldSize(scale) / util::DEGREES_MAX;
    Cc = Projection::worldSize(scale) / util::M2PI;
}

} // namespace mbgl
