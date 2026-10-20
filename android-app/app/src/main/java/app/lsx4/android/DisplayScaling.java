package app.lsx4.android;









final class DisplayScaling {
    static final int CURRENT = 0;
    static final int FIT = 1;
    static final int FILL_CROP = 2;
    static final int STRETCH = 3;
    static final int INTEGER_FIT = 4;
    static final int NATIVE_1_TO_1 = 5;
    static final int ASPECT_4_3 = 6;
    static final int ASPECT_16_9 = 7;
    static final int ASPECT_16_10 = 8;
    static final int ASPECT_21_9 = 9;
    static final int ASPECT_32_9 = 10;

    static final int DEFAULT = CURRENT;

    static final int[] ORDER = {
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

    static final class Dimensions {
        final int width;
        final int height;

        Dimensions(int width, int height) {
            this.width = width;
            this.height = height;
        }
    }

    private DisplayScaling() {
    }

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
        for (int i = 0; i < ORDER.length; ++i) {
            if (ORDER[i] == safeMode) {
                return i;
            }
        }
        return 0;
    }

    static int atPosition(int position) {
        return position >= 0 && position < ORDER.length ? ORDER[position] : DEFAULT;
    }







    static Dimensions measure(int availableWidth, int availableHeight,
                              int sourceWidth, int sourceHeight,
                              int requestedMode, boolean legacyCurrentFits) {
        int width = Math.max(0, availableWidth);
        int height = Math.max(0, availableHeight);
        if (width == 0 || height == 0) {
            return new Dimensions(width, height);
        }

        int mode = sanitize(requestedMode);
        if (mode == CURRENT) {
            mode = legacyCurrentFits ? FIT : STRETCH;
        }
        if (mode == STRETCH) {
            return new Dimensions(width, height);
        }

        boolean hasSourceExtent = sourceWidth > 0 && sourceHeight > 0;
        double sourceAspect = hasSourceExtent
                ? (double) sourceWidth / (double) sourceHeight
                : 16.0 / 9.0;

        switch (mode) {
            case NATIVE_1_TO_1:
                return hasSourceExtent
                        ? new Dimensions(sourceWidth, sourceHeight)
                        : new Dimensions(width, height);
            case INTEGER_FIT:
                if (hasSourceExtent) {
                    double availableScale = Math.min(
                            (double) width / (double) sourceWidth,
                            (double) height / (double) sourceHeight);
                    if (availableScale >= 1.0) {
                        int integerScale = Math.max(1, (int) Math.floor(availableScale));
                        return new Dimensions(sourceWidth * integerScale,
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

    private static Dimensions fit(int width, int height, double aspect) {
        if ((double) width / (double) height > aspect) {
            return new Dimensions(positiveRound(height * aspect), height);
        }
        return new Dimensions(width, positiveRound(width / aspect));
    }

    private static Dimensions fill(int width, int height, double aspect) {
        if ((double) width / (double) height > aspect) {
            return new Dimensions(width, positiveRound(width / aspect));
        }
        return new Dimensions(positiveRound(height * aspect), height);
    }

    private static int positiveRound(double value) {
        return Math.max(1, (int) Math.round(value));
    }
}
