#pragma once
#include <algorithm>
#include <cmath>
#include <iterator>
// Generated from complete Android DisplayScaling.java policy.
/**
 * Pure presentation-geometry policy for the game SurfaceView.
 *
 * <p>This class deliberately does not select a different guest/output resolution or touch the
 * native renderer. It is evaluated only from {@code View.onMeasure()}, so changing the policy
 * adds no work to the per-frame renderer path. A fixed-resolution SurfaceHolder keeps its buffer
 * extent; Native output continues to follow Android's layout-sized surface contract.</p>
 */
struct AndroidDisplayScaling {
    static constexpr int CURRENT = 0;
    static constexpr int FIT = 1;
    static constexpr int FILL_CROP = 2;
    static constexpr int STRETCH = 3;
    static constexpr int INTEGER_FIT = 4;
    static constexpr int NATIVE_1_TO_1 = 5;
    static constexpr int ASPECT_4_3 = 6;
    static constexpr int ASPECT_16_9 = 7;
    static constexpr int ASPECT_16_10 = 8;
    static constexpr int ASPECT_21_9 = 9;
    static constexpr int ASPECT_32_9 = 10;

    static constexpr int DEFAULT = CURRENT;

    inline static constexpr int ORDER[] = {
            CURRENT,
            FIT,
            FILL_CROP,
            STRETCH,
            INTEGER_FIT,
            NATIVE_1_TO_1,
            ASPECT_4_3,
            ASPECT_16_9,
            ASPECT_16_10,
            ASPECT_21_9,
            ASPECT_32_9
    };

    struct Dimensions {
        const int width;
        const int height;

        Dimensions(int width_, int height_) : width(width_), height(height_) {}
    };

    AndroidDisplayScaling() = delete;

    static int sanitize(int mode) {
        for (int candidate : ORDER) {
            if (candidate == mode) {
                return mode;
            }
        }
        return DEFAULT;
    }

    static int positionOf(int mode) {
        int safeMode = sanitize(mode);
        for (int i = 0; i < static_cast<int>(std::size(ORDER)); ++i) {
            if (ORDER[i] == safeMode) {
                return i;
            }
        }
        return 0;
    }

    static int atPosition(int position) {
        return position >= 0 && position < static_cast<int>(std::size(ORDER)) ? ORDER[position] : DEFAULT;
    }

    /**
     * Calculates only the displayed SurfaceView bounds.
     *
     * @param legacyCurrentFits preserves the exact pre-setting behavior: fixed output resolutions
     *                          were aspect-fitted, while native/layout-sized output was stretched.
     */
    static Dimensions measure(int availableWidth, int availableHeight,
                              int sourceWidth, int sourceHeight,
                              int requestedMode, bool legacyCurrentFits) {
        int width = std::max(0, availableWidth);
        int height = std::max(0, availableHeight);
        if (width == 0 || height == 0) {
            return Dimensions(width, height);
        }

        int mode = sanitize(requestedMode);
        if (mode == CURRENT) {
            mode = legacyCurrentFits ? FIT : STRETCH;
        }
        if (mode == STRETCH) {
            return Dimensions(width, height);
        }

        bool hasSourceExtent = sourceWidth > 0 && sourceHeight > 0;
        double sourceAspect = hasSourceExtent
                ? (double) sourceWidth / (double) sourceHeight
                : 16.0 / 9.0;

        switch (mode) {
            case NATIVE_1_TO_1:
                return hasSourceExtent
                        ? Dimensions(sourceWidth, sourceHeight)
                        : Dimensions(width, height);
            case INTEGER_FIT:
                if (hasSourceExtent) {
                    double availableScale = std::min(
                            (double) width / (double) sourceWidth,
                            (double) height / (double) sourceHeight);
                    if (availableScale >= 1.0) {
                        int integerScale = std::max(1, (int) std::floor(availableScale));
                        return Dimensions(sourceWidth * integerScale,
                                sourceHeight * integerScale);
                    }
                }
                return fit(width, height, sourceAspect);
            case FILL_CROP:
                return fill(width, height, sourceAspect);
            case ASPECT_4_3:
                return fit(width, height, 4.0 / 3.0);
            case ASPECT_16_9:
                return fit(width, height, 16.0 / 9.0);
            case ASPECT_16_10:
                return fit(width, height, 16.0 / 10.0);
            case ASPECT_21_9:
                return fit(width, height, 21.0 / 9.0);
            case ASPECT_32_9:
                return fit(width, height, 32.0 / 9.0);
            case FIT:
            default:
                return fit(width, height, sourceAspect);
        }
    }

    static Dimensions fit(int width, int height, double aspect) {
        if ((double) width / (double) height > aspect) {
            return Dimensions(positiveRound(height * aspect), height);
        }
        return Dimensions(width, positiveRound(width / aspect));
    }

    static Dimensions fill(int width, int height, double aspect) {
        if ((double) width / (double) height > aspect) {
            return Dimensions(width, positiveRound(width / aspect));
        }
        return Dimensions(positiveRound(height * aspect), height);
    }

    static int positiveRound(double value) {
        return std::max(1, (int) static_cast<int>(std::lround(value)));
    }
};
