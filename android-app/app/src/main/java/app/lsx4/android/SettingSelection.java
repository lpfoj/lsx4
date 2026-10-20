package app.lsx4.android;


final class SettingSelection {
    private int selected;

    SettingSelection(int selected) { this.selected = selected; }

    boolean changed(int position) {
        if (position < 0 || position == selected) return false;
        selected = position;
        return true;
    }
}
