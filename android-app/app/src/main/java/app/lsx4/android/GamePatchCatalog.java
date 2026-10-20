package app.lsx4.android;

import android.content.Context;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

final class GamePatchCatalog {
    static final class Patch {
        final String id;
        final String preferenceKey;
        final int titleResource;
        final String label;
        final String marker;
        final String group;
        final List<String> conflicts;
        final boolean inverted;

        Patch(String id, String preferenceKey, int titleResource, String label,
              String marker, String group, List<String> conflicts, boolean inverted) {
            this.id = id;
            this.preferenceKey = preferenceKey;
            this.titleResource = titleResource;
            this.label = label;
            this.marker = marker;
            this.group = group;
            this.conflicts = Collections.unmodifiableList(new ArrayList<>(conflicts));
            this.inverted = inverted;
        }

        boolean isChecked(android.content.SharedPreferences preferences) {
            boolean stored = preferences.getBoolean(preferenceKey, false);
            return inverted ? !stored : stored;
        }

        boolean hasMarker() {
            return !marker.isEmpty();
        }

        boolean conflictsWith(Patch peer) {
            return conflicts.contains(peer.id) || peer.conflicts.contains(id);
        }
    }

    static final class Game {
        final String name;
        final List<String> titleIds;
        final List<Patch> patches;

        Game(String name, List<String> titleIds, List<Patch> patches) {
            this.name = name;
            this.titleIds = titleIds;
            this.patches = patches;
        }
    }

    private GamePatchCatalog() {
    }

    static List<Game> load(Context context) {
        try (InputStream input = context.getAssets().open("game_patches.json")) {
            ByteArrayOutputStream output = new ByteArrayOutputStream();
            byte[] buffer = new byte[8192];
            int count;
            while ((count = input.read(buffer)) >= 0) {
                output.write(buffer, 0, count);
            }
            JSONObject root = new JSONObject(output.toString(StandardCharsets.UTF_8.name()));
            JSONArray games = root.getJSONArray("games");
            List<Game> result = new ArrayList<>();
            for (int i = 0; i < games.length(); ++i) {
                JSONObject game = games.getJSONObject(i);
                List<String> titleIds = new ArrayList<>();
                JSONArray ids = game.getJSONArray("titleIds");
                for (int j = 0; j < ids.length(); ++j) {
                    titleIds.add(ids.getString(j));
                }
                List<Patch> patches = new ArrayList<>();
                JSONArray entries = game.getJSONArray("patches");
                for (int j = 0; j < entries.length(); ++j) {
                    JSONObject patch = entries.getJSONObject(j);
                    String titleName = patch.optString("title", "");
                    int title = context.getResources().getIdentifier(
                            titleName, "string", context.getPackageName());
                    String marker = patch.optString("marker", "");
                    String group = patch.optString("group", "");
                    List<String> conflicts = new ArrayList<>();
                    JSONArray conflictEntries = patch.optJSONArray("conflicts");
                    if (conflictEntries != null) {
                        for (int k = 0; k < conflictEntries.length(); ++k) {
                            String conflict = conflictEntries.getString(k);
                            if (!isSafeToken(conflict)) {
                                throw new IllegalArgumentException(
                                        "Unsafe patch conflict for " + patch.getString("id"));
                            }
                            conflicts.add(conflict);
                        }
                    }
                    if (!isSafeToken(marker) || !isSafeToken(group)) {
                        throw new IllegalArgumentException(
                                "Unsafe patch marker/group for " + patch.getString("id"));
                    }
                    patches.add(new Patch(patch.getString("id"),
                            patch.getString("preferenceKey"), title,
                            patch.optString("label", ""), marker, group, conflicts,
                            patch.optBoolean("inverted", true)));
                }
                result.add(new Game(game.getString("name"), titleIds, patches));
            }
            return result;
        } catch (Exception error) {
            android.util.Log.e("LSX4", "Patch catalog could not be loaded", error);
            return Collections.emptyList();
        }
    }

    static List<Patch> allPatches(Context context) {
        List<Patch> result = new ArrayList<>();
        for (Game game : load(context)) {
            result.addAll(game.patches);
        }
        return result;
    }

    static boolean isPreferenceKey(Context context, String key) {
        if (key == null) {
            return false;
        }
        for (Patch patch : allPatches(context)) {
            if (patch.preferenceKey.equals(key)) {
                return true;
            }
        }
        return false;
    }

    private static boolean isSafeToken(String value) {
        if (value.isEmpty()) {
            return true;
        }
        for (int i = 0; i < value.length(); ++i) {
            char character = value.charAt(i);
            if ((character >= 'a' && character <= 'z')
                    || (character >= 'A' && character <= 'Z')
                    || (character >= '0' && character <= '9')
                    || character == '.' || character == '_' || character == '-') {
                continue;
            }
            return false;
        }
        return true;
    }
}
