package app.lsx4.android;

import android.content.SharedPreferences;
import android.graphics.RectF;

import java.util.Arrays;
import java.util.Collections;
import java.util.List;


final class OverlayLayoutStore {
    static final String PREFIX = "overlay_layout_v1_";

    static final class Element {
        final String id;
        final String label;
        final boolean visibleByDefault;

        Element(String id, String label, boolean visibleByDefault) {
            this.id = id;
            this.label = label;
            this.visibleByDefault = visibleByDefault;
        }
    }

    static final Element DPAD = new Element("dpad", "D-Pad", true);
    static final Element FACE = new Element("face", "△ ○ × □", true);
    static final Element LEFT_STICK = new Element("left_stick", "L3", true);
    static final Element RIGHT_STICK = new Element("right_stick", "R3", true);
    static final Element L1 = new Element("l1", "L1", true);
    static final Element L2 = new Element("l2", "L2", true);
    static final Element R1 = new Element("r1", "R1", true);
    static final Element R2 = new Element("r2", "R2", true);
    static final Element OPTIONS = new Element("options", "OPTIONS", true);
    static final Element SHARE = new Element("share", "SHARE", false);
    static final Element PS = new Element("ps", "PS", false);
    static final Element TOUCHPAD = new Element("touchpad", "TOUCH PAD", false);

    static final List<Element> ELEMENTS = Collections.unmodifiableList(Arrays.asList(
            DPAD, FACE, LEFT_STICK, RIGHT_STICK, L1, L2, R1, R2,
            OPTIONS, SHARE, PS, TOUCHPAD));

    static final class State {
        final Element element;
        float offsetX;
        float offsetY;
        float scale;
        boolean visible;

        State(Element element, float offsetX, float offsetY, float scale, boolean visible) {
            this.element = element;
            this.offsetX = offsetX;
            this.offsetY = offsetY;
            this.scale = scale;
            this.visible = visible;
        }

        State copy() {
            return new State(element, offsetX, offsetY, scale, visible);
        }
    }

    private OverlayLayoutStore() {}

    static String visibleKey(Element element) {
        return PREFIX + element.id + "_visible";
    }

    private static String xKey(Element element) {
        return PREFIX + element.id + "_x";
    }

    private static String yKey(Element element) {
        return PREFIX + element.id + "_y";
    }

    private static String scaleKey(Element element) {
        return PREFIX + element.id + "_scale";
    }

    static boolean isPreferenceKey(String key) {
        return key == null || key.startsWith(PREFIX);
    }

    static State read(SharedPreferences preferences, Element element) {
        return new State(element,
                clamp(preferences.getFloat(xKey(element), 0f), -1f, 1f),
                clamp(preferences.getFloat(yKey(element), 0f), -1f, 1f),
                clamp(preferences.getFloat(scaleKey(element), 1f), 0.45f, 2.2f),
                preferences.getBoolean(visibleKey(element), element.visibleByDefault));
    }

    static State[] readAll(SharedPreferences preferences) {
        State[] states = new State[ELEMENTS.size()];
        for (int index = 0; index < states.length; ++index) {
            states[index] = read(preferences, ELEMENTS.get(index));
        }
        return states;
    }

    static State find(State[] states, Element element) {
        if (states != null) {
            for (State state : states) {
                if (state != null && state.element == element) {
                    return state;
                }
            }
        }
        return new State(element, 0f, 0f, 1f, element.visibleByDefault);
    }

    static void write(SharedPreferences preferences, State state) {
        preferences.edit()
                .putFloat(xKey(state.element), clamp(state.offsetX, -1f, 1f))
                .putFloat(yKey(state.element), clamp(state.offsetY, -1f, 1f))
                .putFloat(scaleKey(state.element), clamp(state.scale, 0.45f, 2.2f))
                .putBoolean(visibleKey(state.element), state.visible)
                .apply();
    }

    static void setVisible(SharedPreferences preferences, Element element, boolean visible) {
        preferences.edit().putBoolean(visibleKey(element), visible).apply();
    }

    static void reset(SharedPreferences preferences) {
        SharedPreferences.Editor editor = preferences.edit();
        for (Element element : ELEMENTS) {
            editor.remove(xKey(element));
            editor.remove(yKey(element));
            editor.remove(scaleKey(element));
            editor.remove(visibleKey(element));
        }
        editor.apply();
    }

    static void transform(State state, float hostWidth, float hostHeight, RectF... rects) {
        if (rects == null || rects.length == 0) {
            return;
        }
        RectF bounds = new RectF(rects[0]);
        for (int index = 1; index < rects.length; ++index) {
            bounds.union(rects[index]);
        }
        float centerX = bounds.centerX();
        float centerY = bounds.centerY();
        float offsetX = state.offsetX * hostWidth;
        float offsetY = state.offsetY * hostHeight;
        for (RectF rect : rects) {
            rect.set(centerX + (rect.left - centerX) * state.scale + offsetX,
                    centerY + (rect.top - centerY) * state.scale + offsetY,
                    centerX + (rect.right - centerX) * state.scale + offsetX,
                    centerY + (rect.bottom - centerY) * state.scale + offsetY);
        }
    }

    private static float clamp(float value, float minimum, float maximum) {
        return Math.max(minimum, Math.min(maximum, value));
    }
}
