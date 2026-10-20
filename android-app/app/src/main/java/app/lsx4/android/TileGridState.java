package app.lsx4.android;


final class TileGridState {
    private int count;
    private int selected;
    private float offset;
    private float velocity;
    private float visibleGroups = 1f;
    private float tileSpan = 1f;

    int selected() { return selected; }
    float offset() { return offset; }

    void setItems(int count, int selected, boolean preserveViewport) {
        this.count = Math.max(0, count);
        this.selected = clampIndex(selected);
        stopMotion();
        if (!preserveViewport) offset = this.selected / 2;
        offset = bound(offset);
    }

    void setViewport(float visibleGroups, float tileSpan) {
        this.visibleGroups = Math.max(0.1f, visibleGroups);
        this.tileSpan = Math.max(0.1f, Math.min(1f, tileSpan));
        offset = bound(offset);
    }

    void select(int index, boolean reveal) {
        selected = clampIndex(index);
        stopMotion();
        if (reveal) {
            float group = selected / 2;
            if (group < offset) offset = group;
            else if (group + tileSpan > offset + visibleGroups)
                offset = group + tileSpan - visibleGroups;
            offset = bound(offset);
        }
    }

    void scrollBy(float groups) {
        stopMotion();
        if (Float.isNaN(groups) || Float.isInfinite(groups)) return;
        offset = bound(offset + groups);
    }

    void fling(float groupsPerSecond) {
        if (Float.isNaN(groupsPerSecond) || Float.isInfinite(groupsPerSecond)) return;
        velocity = Math.max(-40f, Math.min(40f, groupsPerSecond));
    }

    void stopMotion() { velocity = 0f; }

    void step(float seconds) {
        if (seconds <= 0f || Float.isNaN(seconds) || Float.isInfinite(seconds)) return;
        float dt = Math.min(0.05f, seconds);
        float wanted = offset + velocity * dt;
        offset = bound(wanted);
        if (offset != wanted) velocity = 0f;
        else velocity *= (float)Math.exp(-6f * dt);
        if (Math.abs(velocity) < 0.01f) velocity = 0f;
    }

    private int clampIndex(int index) { return Math.max(0, Math.min(Math.max(0, count - 1), index)); }

    private float bound(float value) {
        int groups = (count + 1) / 2;
        float extent = groups == 0 ? 0f : groups - 1 + tileSpan;
        return Math.max(0f, Math.min(Math.max(0f, extent - visibleGroups), value));
    }
}
