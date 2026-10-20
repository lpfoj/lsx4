package app.lsx4.android;



final class GamePlatformMetadata {
    private GamePlatformMetadata() {}
    static boolean isPs5TitleId(String titleId) {
        if (titleId == null || titleId.length() != 9 || !titleId.startsWith("PPSA")) return false;
        for (int i = 4; i < 9; i++) {
            if (titleId.charAt(i) < '0' || titleId.charAt(i) > '9') return false;
        }
        return true;
    }
}
