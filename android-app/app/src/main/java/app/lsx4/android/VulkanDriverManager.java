package app.lsx4.android;

import android.content.Context;
import android.content.SharedPreferences;
import android.net.Uri;
import android.os.Build;

import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.AtomicMoveNotSupportedException;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.Comparator;
import java.util.Enumeration;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.UUID;
import java.util.regex.Matcher;
import java.util.regex.Pattern;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;

final class VulkanDriverManager {
    static final int DRIVER_QUALCOMM = 0;
    static final int DRIVER_MESA_TURNIP = 1;
    static final String PREF_DRIVER_SELECTION = "managed_vulkan_driver";
    static final String PREF_DRIVER_SELECTION_ID =
            "managed_vulkan_driver_selection_v2";
    static final String LEGACY_PREF_TURNIP = "managed_turnip_vulkan_driver";
    static final String DRIVER_FILE_NAME = "libvulkan_lsx4_turnip.so";
    static final String SELECTION_SYSTEM = "system";
    static final String SELECTION_BUNDLED = "bundled";
    static final String SELECTION_IMPORTED_PREFIX = "imported:";
    static final String BUNDLED_TURNIP_VERSION = "26.2.1";
    static final String BUNDLED_TURNIP_SHA256 =
            "b56a4cec9af33af61ec068efb1d8673b54bb325635ca7a2300db77df82d752cd";

    private static final String ROOT_DIRECTORY = "vulkan-drivers";
    private static final String SLOTS_DIRECTORY = "slots";
    private static final String ACTIVE_FILE = "active.json";
    private static final String ACTIVE_SCHEMA = "lsx4.turnip-driver-active.v2";
    private static final String PACKAGE_SCHEMA = "lsx4.turnip-driver-package.v1";
    private static final String VERIFIED_FILE = "verified.json";
    private static final String VERIFIED_SCHEMA = "lsx4.turnip-driver-verification.v1";
    private static final int PROBE_CONTRACT_VERSION = 3;
    private static final long QUALCOMM_VULKAN_VENDOR_ID = 0x5143L;
    private static final long ADRENO_750_VULKAN_DEVICE_ID = 0x43051401L;
    private static final long ADRENO_840_VULKAN_DEVICE_ID = 0x44050a31L;
    private static final String VULKAN_VENDOR_ID_FIELD = "vulkanVendorId";
    private static final String VULKAN_DEVICE_ID_FIELD = "vulkanDeviceId";
    private static final String VULKAN_HARDWARE_KEY_FIELD = "vulkanHardwareKey";
    private static final File KGSL_GPU_MODEL_FILE =
            new File("/sys/class/kgsl/kgsl-3d0/gpu_model");
    private static final String RECOVERY_FILE = "recovery.json";
    private static final int MAX_CONSECUTIVE_FAILURES = 3;
    private static final long MAX_ARCHIVE_BYTES = 256L * 1024L * 1024L;
    private static final long MAX_LIBRARY_BYTES = 192L * 1024L * 1024L;
    private static final long MAX_EXPANDED_BYTES = 384L * 1024L * 1024L;
    private static final int MAX_ARCHIVE_ENTRIES = 256;
    private static final int MAX_METADATA_BYTES = 64 * 1024;
    private static final Object IMPORT_LOCK = new Object();
    private static DriverPackage bundledCache;
    private static final Pattern DISPLAY_VERSION = Pattern.compile(
            "(?i)(?:mesa\\s*)?(\\d+\\.\\d+(?:\\.\\d+)?(?:[-_][a-z0-9.]+)?)");

    private static final class HardwareIdentity {
        final long vendorId;
        final long deviceId;

        HardwareIdentity(long vendorId, long deviceId) {
            this.vendorId = vendorId;
            this.deviceId = deviceId;
        }

        String key() {
            return String.format(Locale.ROOT, "v1-%08x-%08x", vendorId, deviceId);
        }

        boolean matches(HardwareIdentity other) {
            return other != null && vendorId == other.vendorId && deviceId == other.deviceId;
        }
    }

    static final class DriverPackage {
        final File directory;
        final File library;
        final String sha256;
        final String label;
        final String version;
        final boolean imported;
        final HardwareIdentity hardwareIdentity;

        DriverPackage(File directory, File library, String sha256, String label,
                      String version, boolean imported) {
            this(directory, library, sha256, label, version, imported, null);
        }

        DriverPackage(File directory, File library, String sha256, String label,
                      String version, boolean imported,
                      HardwareIdentity hardwareIdentity) {
            this.directory = directory;
            this.library = library;
            this.sha256 = sha256;
            this.label = label;
            this.version = version;
            this.imported = imported;
            this.hardwareIdentity = hardwareIdentity;
        }

        String summary() {
            StringBuilder value = new StringBuilder(label);
            if (!version.isEmpty() && !label.toLowerCase(Locale.ROOT)
                    .contains(version.toLowerCase(Locale.ROOT))) {
                value.append(' ').append(version);
            }
            if (!sha256.isEmpty()) {
                value.append(" · ").append(sha256.substring(0, Math.min(12, sha256.length())));
            }
            return value.toString();
        }
    }

    static final class ImportResult {
        final DriverPackage driverPackage;
        final int libraryCount;
        final boolean newSlot;

        ImportResult(DriverPackage driverPackage, int libraryCount, boolean newSlot) {
            this.driverPackage = driverPackage;
            this.libraryCount = libraryCount;
            this.newSlot = newSlot;
        }
    }

    static final class DriverEntry {
        final String selectionId;
        final DriverPackage driverPackage;
        final String version;

        DriverEntry(String selectionId, DriverPackage driverPackage, String version) {
            this.selectionId = selectionId;
            this.driverPackage = driverPackage;
            this.version = version;
        }

        boolean isSystem() {
            return SELECTION_SYSTEM.equals(selectionId);
        }

        boolean isBundled() {
            return SELECTION_BUNDLED.equals(selectionId);
        }

        boolean isImported() {
            return selectionId.startsWith(SELECTION_IMPORTED_PREFIX);
        }
    }

    static final class LaunchDecision {
        final DriverPackage driverPackage;
        final boolean recoveredToSystem;
        final String message;

        LaunchDecision(DriverPackage driverPackage, boolean recoveredToSystem,
                       String message) {
            this.driverPackage = driverPackage;
            this.recoveredToSystem = recoveredToSystem;
            this.message = message;
        }
    }

    private static final class ArchiveLibrary {
        final ZipEntry entry;
        final String baseName;
        final int driverPriority;

        ArchiveLibrary(ZipEntry entry, String baseName, int driverPriority) {
            this.entry = entry;
            this.baseName = baseName;
            this.driverPriority = driverPriority;
        }
    }

    private VulkanDriverManager() {
    }


    static int readSelection(SharedPreferences preferences) {
        Map<String, ?> values = preferences.getAll();
        Object current = values.get(PREF_DRIVER_SELECTION);
        if (current instanceof Number) {
            return sanitizeSelection(((Number) current).intValue());
        }
        int migrated = DRIVER_QUALCOMM;
        Object legacy = values.get(LEGACY_PREF_TURNIP);
        if (legacy instanceof Boolean && (Boolean) legacy) {
            migrated = DRIVER_MESA_TURNIP;
        }
        preferences.edit().putInt(PREF_DRIVER_SELECTION, migrated)
                .remove(LEGACY_PREF_TURNIP).apply();
        return migrated;
    }

    static String readSelectionId(Context context, SharedPreferences preferences) {
        synchronized (IMPORT_LOCK) {
            Map<String, ?> values = preferences.getAll();
            Object current = values.get(PREF_DRIVER_SELECTION_ID);
            if (current instanceof String) {
                String rawSelection = (String) current;
                String selectionId = sanitizeSelectionId(rawSelection);
                boolean wellFormed = rawSelection.equals(SELECTION_SYSTEM)
                        || rawSelection.equals(SELECTION_BUNDLED)
                        || rawSelection.matches(
                        SELECTION_IMPORTED_PREFIX + "[0-9a-fA-F]{64}");
                if (wellFormed && selectionIdExists(context, selectionId)) {
                    if (!rawSelection.equals(selectionId)) {
                        persistSelection(preferences, selectionId);
                    }
                    return selectionId;
                }
                recoverUnavailableSelection(context, preferences, rawSelection,
                        "selection-not-found-or-changed");
                return SELECTION_SYSTEM;
            }

            int legacySelection = readSelection(preferences);
            String migrated = legacySelection == DRIVER_MESA_TURNIP
                    ? SELECTION_BUNDLED : SELECTION_SYSTEM;


            persistSelection(preferences, migrated);
            return migrated;
        }
    }

    static void writeSelection(SharedPreferences preferences, int selection) {
        persistSelection(preferences, selection == DRIVER_MESA_TURNIP
                ? SELECTION_BUNDLED : SELECTION_SYSTEM);
    }

    static int sanitizeSelection(int selection) {
        return selection == DRIVER_MESA_TURNIP
                ? DRIVER_MESA_TURNIP : DRIVER_QUALCOMM;
    }

    static List<DriverEntry> listDrivers(Context context) {
        synchronized (IMPORT_LOCK) {
            List<DriverEntry> result = new ArrayList<>();
            result.add(new DriverEntry(SELECTION_SYSTEM, null, ""));
            if (currentHardwareIdentity(context) == null) {
                return result;
            }
            DriverPackage bundled = bundledDriver(context);
            if (bundled != null) {
                result.add(new DriverEntry(SELECTION_BUNDLED, bundled,
                        BUNDLED_TURNIP_VERSION));
            }
            List<DriverEntry> imported = new ArrayList<>();
            File slots = new File(driverRoot(context), SLOTS_DIRECTORY);
            File[] children = slots.listFiles();
            if (children != null) {
                for (File child : children) {
                    String sha = child.getName().toLowerCase(Locale.ROOT);
                    if (!sha.matches("[0-9a-f]{64}")) {
                        continue;
                    }
                    try {
                        DriverPackage driver = readVerifiedImportedMetadata(context, sha);
                        if (driver != null) {
                            imported.add(new DriverEntry(importedSelectionId(sha), driver,
                                    displayVersion(driver)));
                        }
                    } catch (Exception ignored) {
                    }
                }
            }
            imported.sort(Comparator
                    .comparing((DriverEntry entry) -> entry.version,
                            String.CASE_INSENSITIVE_ORDER)
                    .thenComparing(entry -> entry.driverPackage.sha256));
            result.addAll(imported);
            return result;
        }
    }

    static DriverPackage resolveSelection(Context context, String selectionId) {
        synchronized (IMPORT_LOCK) {
            String selected = sanitizeSelectionId(selectionId);
            if (SELECTION_SYSTEM.equals(selected)) {
                return null;
            }
            if (currentHardwareIdentity(context) == null) {
                return null;
            }
            if (SELECTION_BUNDLED.equals(selected)) {
                return bundledDriver(context);
            }
            if (selected.startsWith(SELECTION_IMPORTED_PREFIX)) {
                try {
                    return readVerifiedImported(context,
                            selected.substring(SELECTION_IMPORTED_PREFIX.length()));
                } catch (Exception ignored) {
                    return null;
                }
            }
            return null;
        }
    }

    static void selectDriver(Context context, SharedPreferences preferences,
                             String selectionId) throws Exception {
        synchronized (IMPORT_LOCK) {
            String selected = sanitizeSelectionId(selectionId);
            if (!selectionIdExists(context, selected)) {
                throw new IOException("Selected Vulkan driver is no longer available");
            }
            if (selected.startsWith(SELECTION_IMPORTED_PREFIX)) {
                String sha = selected.substring(SELECTION_IMPORTED_PREFIX.length());
                DriverPackage driver = readVerifiedImported(context, sha);
                if (driver == null) {
                    throw new IOException("Imported Vulkan driver failed verification");
                }





                activateSelectedImported(context, driver, true);
            } else {
                File root = driverRoot(context);
                deleteIfPresent(new File(root, ACTIVE_FILE));
            }
            if (!persistSelection(preferences, selected)) {
                throw new IOException("Could not persist the Vulkan driver selection");
            }
            clearRecoveryNotice(context);
        }
    }

    private static boolean persistSelection(SharedPreferences preferences,
                                            String selectionId) {
        String selected = sanitizeSelectionId(selectionId);
        int legacy = SELECTION_SYSTEM.equals(selected)
                ? DRIVER_QUALCOMM : DRIVER_MESA_TURNIP;
        return preferences.edit()
                .putString(PREF_DRIVER_SELECTION_ID, selected)
                .putInt(PREF_DRIVER_SELECTION, legacy)
                .remove(LEGACY_PREF_TURNIP)
                .commit();
    }

    private static String sanitizeSelectionId(String selectionId) {
        if (SELECTION_SYSTEM.equals(selectionId) || SELECTION_BUNDLED.equals(selectionId)) {
            return selectionId;
        }
        if (selectionId != null && selectionId.matches(
                SELECTION_IMPORTED_PREFIX + "[0-9a-fA-F]{64}")) {
            return SELECTION_IMPORTED_PREFIX
                    + selectionId.substring(SELECTION_IMPORTED_PREFIX.length())
                    .toLowerCase(Locale.ROOT);
        }
        return SELECTION_SYSTEM;
    }

    private static boolean selectionIdExists(Context context, String selectionId) {
        if (SELECTION_SYSTEM.equals(selectionId)) {
            return true;
        }
        if (SELECTION_BUNDLED.equals(selectionId)) {
            return bundledDriver(context) != null;
        }
        if (selectionId.startsWith(SELECTION_IMPORTED_PREFIX)) {
            try {
                return readVerifiedImportedMetadata(context,
                        selectionId.substring(SELECTION_IMPORTED_PREFIX.length())) != null;
            } catch (Exception ignored) {
                return false;
            }
        }
        return false;
    }

    private static String importedSelectionId(String sha256) {
        return SELECTION_IMPORTED_PREFIX + sha256.toLowerCase(Locale.ROOT);
    }

    private static String displayVersion(DriverPackage driver) {
        Matcher matcher = DISPLAY_VERSION.matcher(
                driver.version + " " + driver.label);
        if (matcher.find()) {
            return matcher.group(1).replace('_', '-');
        }
        return driver.sha256.substring(0, Math.min(8, driver.sha256.length()));
    }

    private static HardwareIdentity supportedHardwareIdentity(long vendorId,
                                                               long deviceId) {
        long normalizedVendor = vendorId & 0xffffffffL;
        long normalizedDevice = deviceId & 0xffffffffL;
        if (normalizedVendor != QUALCOMM_VULKAN_VENDOR_ID
                || (normalizedDevice != ADRENO_750_VULKAN_DEVICE_ID
                && normalizedDevice != ADRENO_840_VULKAN_DEVICE_ID)) {
            return null;
        }
        return new HardwareIdentity(normalizedVendor, normalizedDevice);
    }






    static boolean isBundledTurnipSupportedHardware(long vendorId, long deviceId) {
        return (vendorId & 0xffffffffL) == QUALCOMM_VULKAN_VENDOR_ID
                && ((deviceId & 0xffffffffL) == ADRENO_750_VULKAN_DEVICE_ID
                || (deviceId & 0xffffffffL) == ADRENO_840_VULKAN_DEVICE_ID);
    }

    private static String normalizedGpuSignal(String value) {
        return value == null ? "" : value.toLowerCase(Locale.ROOT)
                .replaceAll("[^a-z0-9]", "");
    }

    private static HardwareIdentity hardwareIdentityFromGpuSignal(String value) {
        String normalized = normalizedGpuSignal(value);
        if (normalized.matches("(?:turnip)?adreno(?:tm)?750(?:v[0-9]+)?")) {
            return supportedHardwareIdentity(
                    QUALCOMM_VULKAN_VENDOR_ID, ADRENO_750_VULKAN_DEVICE_ID);
        }
        if (normalized.matches("(?:turnip)?adreno(?:tm)?840(?:v[0-9]+)?")) {
            return supportedHardwareIdentity(
                    QUALCOMM_VULKAN_VENDOR_ID, ADRENO_840_VULKAN_DEVICE_ID);
        }
        return null;
    }

    private static HardwareIdentity hardwareIdentityFromSocModel(String value) {
        String normalized = normalizedGpuSignal(value);
        if ("sm8650".equals(normalized)) {
            return supportedHardwareIdentity(
                    QUALCOMM_VULKAN_VENDOR_ID, ADRENO_750_VULKAN_DEVICE_ID);
        }
        if ("sm8850".equals(normalized)) {
            return supportedHardwareIdentity(
                    QUALCOMM_VULKAN_VENDOR_ID, ADRENO_840_VULKAN_DEVICE_ID);
        }
        return null;
    }

    private static HardwareIdentity currentHardwareIdentity(Context context) {
        if (!isAdrenoPlatform()) {
            return null;
        }
        try {
            if (KGSL_GPU_MODEL_FILE.isFile() && KGSL_GPU_MODEL_FILE.canRead()) {
                HardwareIdentity identity = hardwareIdentityFromGpuSignal(
                        readFileTextLimited(KGSL_GPU_MODEL_FILE, 256));
                if (identity != null) {
                    return identity;
                }
            }
        } catch (Exception ignored) {
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            return hardwareIdentityFromSocModel(Build.SOC_MODEL);
        }
        return null;
    }







    static String runtimeHardwareProfile(Context context, String driverStatus) {
        HardwareIdentity identity = currentHardwareIdentity(context);
        if (identity != null) {
            if (identity.deviceId == ADRENO_750_VULKAN_DEVICE_ID) {
                return "adreno-750";
            }
            if (identity.deviceId == ADRENO_840_VULKAN_DEVICE_ID) {
                return "adreno-840";
            }
        }
        String signal = driverStatus == null ? "" : driverStatus.toLowerCase(Locale.ROOT);
        if (signal.contains("mali") || signal.contains("arm proprietary")) {
            return "mali";
        }
        return "unknown";
    }

    static String runtimeDriverFamily(String driverStatus, String hardwareProfile) {
        try {
            JSONObject status = new JSONObject(driverStatus == null ? "{}" : driverStatus);
            String requested = status.optString("requested", "");
            boolean selectionLocked = status.optBoolean("selectionLocked", false);






            if ("turnip".equals(requested) && !selectionLocked) {
                return "system-unknown";
            }
            String identity = (status.optString("driverName", "") + " "
                    + status.optString("driverInfo", "") + " "
                    + status.optString("effective", "")).toLowerCase(Locale.ROOT);
            if (identity.contains("turnip") || identity.contains("mesa")) {
                return "mesa-turnip";
            }
            if (identity.contains("qualcomm") || identity.contains("adreno")) {
                return "qualcomm-proprietary";
            }
            if (identity.contains("mali") || identity.contains("arm proprietary")) {
                return "arm-proprietary";
            }




            if ("system".equals(status.optString("effective", ""))) {
                if ("adreno-750".equals(hardwareProfile)
                        || "adreno-840".equals(hardwareProfile)) {
                    return "qualcomm-proprietary";
                }
                if ("mali".equals(hardwareProfile)) {
                    return "arm-proprietary";
                }
            }
        } catch (Exception ignored) {
        }
        return "system-unknown";
    }

    static String runtimeDriverVersion(String driverStatus) {
        try {
            JSONObject status = new JSONObject(driverStatus == null ? "{}" : driverStatus);
            String version = status.optString("driverVersion", "").trim();
            if (version.isEmpty()) {
                version = status.optString("driverInfo", "").trim();
            }
            if (version.isEmpty()) {
                version = status.optString("sha256", "").trim();
            }
            return version.length() <= 160 ? version : version.substring(0, 160);
        } catch (Exception ignored) {
            return "";
        }
    }

    private static HardwareIdentity requireCurrentHardwareIdentity(Context context)
            throws IOException {
        HardwareIdentity identity = currentHardwareIdentity(context);
        if (identity == null) {
            throw new IOException(
                    "Mesa Turnip is supported only on identified Adreno 750/840 hardware");
        }
        return identity;
    }

    private static boolean hasAny(JSONObject json, String... fields) {
        for (String field : fields) {
            if (json.has(field) && !json.isNull(field)) {
                return true;
            }
        }
        return false;
    }

    private static Long readUnsignedId(JSONObject json, String... fields) {
        for (String field : fields) {
            if (!json.has(field) || json.isNull(field)) {
                continue;
            }
            Object raw = json.opt(field);
            try {
                if (raw instanceof Number) {
                    return ((Number) raw).longValue() & 0xffffffffL;
                }
                String value = String.valueOf(raw).trim().toLowerCase(Locale.ROOT);
                if (value.startsWith("0x")) {
                    return Long.parseUnsignedLong(value.substring(2), 16) & 0xffffffffL;
                }
                return Long.parseUnsignedLong(value, 10) & 0xffffffffL;
            } catch (Exception ignored) {
                return null;
            }
        }
        return null;
    }

    private static HardwareIdentity hardwareIdentityFromProbeStatus(JSONObject status) {
        boolean rawIdentityPresent = hasAny(status,
                VULKAN_VENDOR_ID_FIELD, VULKAN_DEVICE_ID_FIELD,
                "vendorId", "deviceId", "vendorID", "deviceID");
        if (rawIdentityPresent) {
            Long vendorId = readUnsignedId(status,
                    VULKAN_VENDOR_ID_FIELD, "vendorId", "vendorID");
            Long deviceId = readUnsignedId(status,
                    VULKAN_DEVICE_ID_FIELD, "deviceId", "deviceID");
            return vendorId == null || deviceId == null ? null
                    : supportedHardwareIdentity(vendorId, deviceId);
        }



        return hardwareIdentityFromGpuSignal(status.optString("deviceName", ""));
    }

    private static HardwareIdentity hardwareIdentityFromMetadata(JSONObject metadata) {
        Long vendorId = readUnsignedId(metadata, VULKAN_VENDOR_ID_FIELD);
        Long deviceId = readUnsignedId(metadata, VULKAN_DEVICE_ID_FIELD);
        HardwareIdentity identity = vendorId == null || deviceId == null ? null
                : supportedHardwareIdentity(vendorId, deviceId);
        if (identity == null || !identity.key().equals(
                metadata.optString(VULKAN_HARDWARE_KEY_FIELD, ""))) {
            return null;
        }
        return identity;
    }

    private static void putHardwareIdentity(JSONObject destination,
                                            HardwareIdentity identity) throws Exception {
        destination.put(VULKAN_VENDOR_ID_FIELD, identity.vendorId);
        destination.put(VULKAN_DEVICE_ID_FIELD, identity.deviceId);
        destination.put(VULKAN_HARDWARE_KEY_FIELD, identity.key());
    }

    static boolean isAdrenoPlatform() {
        return Build.VERSION.SDK_INT >= Build.VERSION_CODES.P
                && new File("/dev/kgsl-3d0").exists();
    }

    static boolean isLoaderBundleAvailable(Context context) {
        File nativeDir = new File(context.getApplicationInfo().nativeLibraryDir);
        for (String name : new String[]{"libadrenotools.so", "libhook_impl.so",
                "libmain_hook.so"}) {
            File library = new File(nativeDir, name);
            if (!library.isFile() || library.length() <= 0L) {
                return false;
            }
        }
        return true;
    }

    static boolean isMesaAvailable(Context context) {
        return isAdrenoPlatform() && isLoaderBundleAvailable(context)
                && bundledDriver(context) != null;
    }


    static DriverPackage resolveTurnipDriver(Context context) {
        try {
            DriverPackage imported = readActiveImported(context);
            if (imported != null) {
                return imported;
            }
        } catch (Exception ignored) {
        }
        return bundledDriver(context);
    }

    private static DriverPackage bundledDriver(Context context) {
        HardwareIdentity currentHardware = currentHardwareIdentity(context);
        if (currentHardware == null || !isBundledTurnipSupportedHardware(
                currentHardware.vendorId, currentHardware.deviceId)) {
            return null;
        }
        if (bundledCache != null
                && currentHardware.matches(bundledCache.hardwareIdentity)
                && bundledCache.library.isFile()
                && bundledCache.library.length() > 0L) {
            return bundledCache;
        }
        File nativeDir = new File(context.getApplicationInfo().nativeLibraryDir);
        File bundled = new File(nativeDir, DRIVER_FILE_NAME);
        if (!bundled.isFile() || bundled.length() <= 0L) {
            return null;
        }
        try {
            String bundledSha = sha256(bundled);
            if (!BUNDLED_TURNIP_SHA256.equals(bundledSha)) {
                return null;
            }
            ElfVulkanDriverValidator.validate(bundled);
            bundledCache = new DriverPackage(nativeDir, bundled, bundledSha,
                    "Mesa Turnip", BUNDLED_TURNIP_VERSION, false,
                    currentHardware);
            return bundledCache;
        } catch (Exception ignored) {
            return null;
        }
    }

    static DriverPackage activeImported(Context context) {
        try {
            return readActiveImported(context);
        } catch (Exception ignored) {
            return null;
        }
    }

    static void useBundledDriver(Context context) throws IOException {
        synchronized (IMPORT_LOCK) {
            File root = driverRoot(context);
            deleteIfPresent(new File(root, ACTIVE_FILE));
            deleteIfPresent(new File(root, RECOVERY_FILE));
        }
    }

    static ImportResult importZip(Context context, Uri source, String archiveName)
            throws Exception {
        synchronized (IMPORT_LOCK) {
            requireCurrentHardwareIdentity(context);
            File root = driverRoot(context);
            File slots = new File(root, SLOTS_DIRECTORY);
            ensureDirectory(slots);
            File temporaryArchive = new File(context.getCacheDir(),
                    "lsx4-turnip-import-" + UUID.randomUUID() + ".zip");
            File staging = null;
            try {
                copyUriLimited(context, source, temporaryArchive, MAX_ARCHIVE_BYTES);
                try (ZipFile archive = new ZipFile(temporaryArchive)) {
                    ArchiveLibrary driver = null;
                    ZipEntry metadataEntry = null;
                    java.util.HashSet<String> entryNames = new java.util.HashSet<>();
                    int entryCount = 0;
                    Enumeration<? extends ZipEntry> entries = archive.entries();
                    while (entries.hasMoreElements()) {
                        ZipEntry entry = entries.nextElement();
                        if (++entryCount > MAX_ARCHIVE_ENTRIES) {
                            throw new IOException("Driver archive has too many entries");
                        }
                        validateArchivePath(entry.getName(), entry.isDirectory());
                        String nameKey = entry.getName().toLowerCase(Locale.ROOT);
                        if (!entryNames.add(nameKey)) {
                            throw new IOException("Driver archive contains duplicate paths");
                        }
                        if (isMacOsJunk(entry.getName())) {
                            continue;
                        }
                        if (entry.isDirectory()) {
                            throw new IOException("Driver archive contains an unexpected directory");
                        }
                        if (entry.getName().indexOf('/') >= 0) {
                            throw new IOException("Driver archive files must be at the archive root");
                        }
                        String baseName = archiveBaseName(entry.getName());
                        String lower = baseName.toLowerCase(Locale.ROOT);
                        if ("meta.json".equals(lower)) {
                            if (metadataEntry != null || entry.getSize() > MAX_METADATA_BYTES) {
                                throw new IOException("Driver archive metadata is duplicate or too large");
                            }
                            metadataEntry = entry;
                            continue;
                        }
                        if (!lower.endsWith(".so")) {
                            if (!lower.endsWith(".json")
                                    || entry.getSize() > MAX_METADATA_BYTES) {
                                throw new IOException("Unexpected file in driver archive");
                            }
                            continue;
                        }
                        if (driver != null) {
                            throw new IOException("Driver archive must contain exactly one library");
                        }
                        if (!isSafeLibraryName(baseName)) {
                            throw new IOException("Unsafe library filename in driver archive");
                        }
                        if (!zipEntryIsAarch64Elf(archive, entry)) {
                            throw new IOException("Driver library is not an Android arm64 ELF");
                        }
                        if (driverPriority(lower) < 0) {
                            throw new IOException("Archive library is not a recognized Turnip driver");
                        }
                        driver = new ArchiveLibrary(entry, baseName, driverPriority(lower));
                    }
                    if (metadataEntry == null || driver == null) {
                        throw new IOException(
                                "Driver archive requires root meta.json and exactly one library");
                    }
                    String metadata =
                            readZipTextLimited(archive, metadataEntry, MAX_METADATA_BYTES);
                    validatePackageMetadata(metadata, driver.baseName);
                    String[] identity = driverIdentity(metadata, archiveName);

                    staging = new File(slots, ".importing-" + UUID.randomUUID());
                    ensureDirectory(staging);
                    long[] expanded = {0L};
                    File stagedDriver = new File(staging, DRIVER_FILE_NAME);
                    copyZipEntryLimited(archive, driver.entry, stagedDriver, expanded);
                    if (!isAarch64Elf(stagedDriver)) {
                        throw new IOException("Imported library changed during extraction");
                    }
                    stagedDriver.setReadable(true, true);
                    stagedDriver.setExecutable(true, true);
                    ElfVulkanDriverValidator.validate(stagedDriver);
                    if (containsAnyAscii(stagedDriver, "libc.so.6", "libstdc++.so.6",
                            "ld-linux-aarch64.so.1", "libX11.so", "libxcb.so")) {
                        throw new IOException("Linux/glibc Vulkan drivers are not supported on Android");
                    }
                    String driverSha = sha256(stagedDriver);
                    JSONObject packageMetadata = new JSONObject();
                    packageMetadata.put("schema", PACKAGE_SCHEMA);
                    packageMetadata.put("sha256", driverSha);
                    packageMetadata.put("driverFile", DRIVER_FILE_NAME);
                    packageMetadata.put("label", identity[0]);
                    packageMetadata.put("version", identity[1]);
                    packageMetadata.put("archiveName", safeIdentity(archiveName, "driver.zip"));
                    packageMetadata.put("originalEntry", driver.entry.getName());
                    packageMetadata.put("libraryCount", 1);
                    writeJsonSynced(new File(staging, "package.json"), packageMetadata);

                    File destination = new File(slots, driverSha);
                    boolean newSlot = false;
                    if (destination.exists()) {
                        DriverPackage existing = readSlot(destination, driverSha,
                                identity[0], identity[1]);
                        if (existing == null) {
                            throw new IOException("Existing driver slot failed validation");
                        }
                        deleteTree(root, staging);
                        staging = null;
                    } else {
                        atomicMove(staging, destination);
                        staging = null;
                        newSlot = true;
                    }
                    DriverPackage installed = readSlot(destination, driverSha,
                            identity[0], identity[1]);
                    if (installed == null) {
                        throw new IOException("Installed driver slot failed verification");
                    }
                    return new ImportResult(installed, 1, newSlot);
                }
            } finally {
                if (staging != null && staging.exists()) {
                    deleteTree(root, staging);
                }
                if (temporaryArchive.exists()) {
                    temporaryArchive.delete();
                }
            }
        }
    }

    static DriverPackage registerImported(Context context, DriverPackage candidate,
                                          String probeStatus) throws Exception {
        synchronized (IMPORT_LOCK) {
            File root = driverRoot(context);
            ensureDirectory(root);
            DriverPackage verifiedSlot = readSlot(candidate.directory, candidate.sha256,
                    candidate.label, candidate.version);
            if (verifiedSlot == null) {
                throw new IOException("Probed driver slot changed before registration");
            }
            HardwareIdentity currentHardware = requireCurrentHardwareIdentity(context);
            JSONObject status = validateProbeStatus(verifiedSlot, probeStatus);
            HardwareIdentity probedHardware = hardwareIdentityFromMetadata(status);
            if (!currentHardware.matches(probedHardware)) {
                throw new IOException(
                        "Driver probe hardware does not match the current device");
            }
            String driverName = safeIdentity(status.optString("driverName"),
                    "Mesa Turnip");
            String driverInfo = safeIdentity(status.optString("driverInfo"), "");
            String driverVersion = safeIdentity(status.optString("driverVersion"), "");
            JSONObject verification = new JSONObject();
            verification.put("schema", VERIFIED_SCHEMA);
            verification.put("probeContractVersion", PROBE_CONTRACT_VERSION);
            verification.put("slot", verifiedSlot.sha256);
            verification.put("sha256", verifiedSlot.sha256);
            verification.put("label", driverName);
            verification.put("version",
                    driverInfo.isEmpty() ? driverVersion : driverInfo);
            verification.put("driverName", driverName);
            verification.put("driverInfo", driverInfo);
            verification.put("driverVersion", driverVersion);
            verification.put("deviceFingerprint", Build.FINGERPRINT);
            verification.put("deviceModel", Build.MODEL);
            verification.put("sdk", Build.VERSION.SDK_INT);
            putHardwareIdentity(verification, currentHardware);
            verification.put("librarySize", verifiedSlot.library.length());
            verification.put("libraryLastModifiedMs",
                    verifiedSlot.library.lastModified());
            verification.put("probeStatus", status);
            verification.put("probeScope", "identity-loadability-no-screen-present");
            verification.put("verifiedAtMs", System.currentTimeMillis());
            writeJsonAtomic(root, new File(verifiedSlot.directory, VERIFIED_FILE),
                    verification);
            DriverPackage installed = readVerifiedImported(
                    context, verifiedSlot.sha256);
            if (installed == null) {
                throw new IOException("Registered driver could not be read back");
            }
            return installed;
        }
    }


    static DriverPackage activateImported(Context context, DriverPackage candidate,
                                          String probeStatus) throws Exception {
        return registerImported(context, candidate, probeStatus);
    }

    static DriverPackage validateProbeTarget(Context context, String directoryPath,
                                             String expectedSha) throws Exception {
        requireCurrentHardwareIdentity(context);
        String sha = expectedSha == null ? ""
                : expectedSha.toLowerCase(Locale.ROOT);
        if (!sha.matches("[0-9a-f]{64}")) {
            throw new IOException("Probe target SHA-256 is invalid");
        }
        File slots = new File(driverRoot(context), SLOTS_DIRECTORY).getCanonicalFile();
        File directory = new File(directoryPath == null ? "" : directoryPath)
                .getCanonicalFile();
        if (!isInside(slots, directory) || !sha.equals(directory.getName())
                || !slots.equals(directory.getParentFile())) {
            throw new IOException("Probe target is outside the immutable driver store");
        }
        DriverPackage candidate = readSlot(directory, sha, "Imported Mesa Turnip", "");
        if (candidate == null) {
            throw new IOException("Probe target failed its content-addressed slot check");
        }
        ElfVulkanDriverValidator.validate(candidate.library);
        return candidate;
    }

    static void discardInactiveImport(Context context, ImportResult result) {
        if (result == null || !result.newSlot || result.driverPackage == null) {
            return;
        }
        synchronized (IMPORT_LOCK) {
            try {
                File root = driverRoot(context);
                JSONObject active = readActiveJson(root);
                HardwareIdentity currentHardware = currentHardwareIdentity(context);
                if (active != null && result.driverPackage.sha256.equalsIgnoreCase(
                        active.optString("sha256", ""))
                        && currentHardware != null
                        && currentHardware.matches(
                        hardwareIdentityFromMetadata(active))) {
                    return;
                }
                if (result.driverPackage.directory.exists()) {
                    deleteTree(root, result.driverPackage.directory);
                }
            } catch (Exception ignored) {
            }
        }
    }

    static LaunchDecision beginGameLaunch(Context context, DriverPackage requested)
            throws Exception {
        if (requested == null || !requested.imported) {
            return new LaunchDecision(requested, false, "");
        }
        synchronized (IMPORT_LOCK) {
            HardwareIdentity currentHardware = requireCurrentHardwareIdentity(context);
            if (requested.hardwareIdentity == null
                    || !currentHardware.matches(requested.hardwareIdentity)) {
                throw new IOException(
                        "Selected Vulkan driver is verified for different hardware");
            }
            File root = driverRoot(context);
            JSONObject active = readActiveJson(root);
            if (active == null || !requested.sha256.equalsIgnoreCase(
                    active.optString("sha256", ""))
                    || !requested.hardwareIdentity.matches(
                    hardwareIdentityFromMetadata(active))) {



                activateSelectedImported(context, requested, false);
                active = readActiveJson(root);
                if (active == null || !requested.sha256.equalsIgnoreCase(
                        active.optString("sha256", ""))
                        || !requested.hardwareIdentity.matches(
                        hardwareIdentityFromMetadata(active))) {
                    throw new IOException(
                            "Could not arm the selected imported Vulkan driver");
                }
            }
            int failures = Math.max(0, active.optInt("consecutiveFailures", 0));
            if (active.optBoolean("launchPending", false)) {
                failures++;
            }
            if (failures >= MAX_CONSECUTIVE_FAILURES) {
                String message = "Mesa Turnip was disabled after " + failures
                        + " consecutive unconfirmed game launches";
                rollbackAfterFailures(root, active, message);
                return new LaunchDecision(null, true, message);
            }
            active.put("consecutiveFailures", failures);
            active.put("launchPending", true);
            active.put("launchStartedAtMs", System.currentTimeMillis());
            writeJsonAtomic(root, new File(root, ACTIVE_FILE), active);
            JSONObject persisted = readActiveJson(root);
            if (persisted == null || !persisted.optBoolean("launchPending", false)
                    || !requested.sha256.equalsIgnoreCase(
                    persisted.optString("sha256", ""))
                    || !requested.hardwareIdentity.matches(
                    hardwareIdentityFromMetadata(persisted))) {
                throw new IOException("Could not persist the driver launch marker");
            }
            return new LaunchDecision(requested, false, "");
        }
    }

    static void confirmGameLaunch(Context context, String sha256) {
        if (sha256 == null || sha256.isEmpty()) {
            return;
        }
        synchronized (IMPORT_LOCK) {
            try {
                File root = driverRoot(context);
                JSONObject active = readActiveJson(root);
                if (active == null || !sha256.equalsIgnoreCase(
                        active.optString("sha256", ""))) {
                    return;
                }
                HardwareIdentity currentHardware = currentHardwareIdentity(context);
                if (currentHardware == null || !currentHardware.matches(
                        hardwareIdentityFromMetadata(active))) {
                    return;
                }
                active.put("consecutiveFailures", 0);
                active.put("launchPending", false);
                active.put("lastConfirmedAtMs", System.currentTimeMillis());
                active.put("screenPresentVerified", true);
                active.put("screenPresentEvidence", "runtime-present-counter");
                writeJsonAtomic(root, new File(root, ACTIVE_FILE), active);
            } catch (Exception ignored) {
            }
        }
    }

    private static String recordUnavailableSelection(Context context, String selectionId,
                                                     String reason) {
        String selected = safeIdentity(selectionId, "unknown");
        String message = "Selected Vulkan driver " + selected
                + " is unavailable or changed; Qualcomm/system was restored";
        try {
            File root = driverRoot(context);
            ensureDirectory(root);
            JSONObject notice = new JSONObject();
            notice.put("schema", "lsx4.turnip-driver-recovery.v1");
            notice.put("message", message);
            notice.put("failedSelectionId", selected);
            if (selected.startsWith(SELECTION_IMPORTED_PREFIX)) {
                notice.put("failedSha256",
                        selected.substring(SELECTION_IMPORTED_PREFIX.length()));
            }
            notice.put("reason", safeIdentity(reason,
                    "selection-not-found-or-changed"));
            notice.put("restored", "system");
            notice.put("createdAtMs", System.currentTimeMillis());
            writeJsonAtomic(root, new File(root, RECOVERY_FILE), notice);
        } catch (Exception ignored) {
        }
        return message;
    }

    static String recoverUnavailableSelection(Context context,
                                              SharedPreferences preferences,
                                              String selectionId,
                                              String reason) {
        synchronized (IMPORT_LOCK) {
            String message = recordUnavailableSelection(context, selectionId, reason);
            try {
                deleteIfPresent(new File(driverRoot(context), ACTIVE_FILE));
            } catch (Exception ignored) {
            }
            persistSelection(preferences, SELECTION_SYSTEM);
            return message;
        }
    }

    static String recoveryNotice(Context context) {
        synchronized (IMPORT_LOCK) {
            try {
                File notice = new File(driverRoot(context), RECOVERY_FILE);
                if (!notice.isFile() || notice.length() <= 0L
                        || notice.length() > MAX_METADATA_BYTES) {
                    return "";
                }
                return safeIdentity(new JSONObject(
                        readFileTextLimited(notice, MAX_METADATA_BYTES))
                        .optString("message"), "");
            } catch (Exception ignored) {
                return "";
            }
        }
    }

    static String consumeRecoveryNotice(Context context, SharedPreferences preferences) {
        synchronized (IMPORT_LOCK) {
            String message = recoveryNotice(context);
            if (message.isEmpty()) {
                return "";
            }
            try {
                deleteIfPresent(new File(driverRoot(context), ACTIVE_FILE));
                if (persistSelection(preferences, SELECTION_SYSTEM)) {
                    deleteIfPresent(new File(driverRoot(context), RECOVERY_FILE));
                }
            } catch (Exception ignored) {
            }
            return message;
        }
    }

    static void clearRecoveryNotice(Context context) {
        synchronized (IMPORT_LOCK) {
            try {
                deleteIfPresent(new File(driverRoot(context), RECOVERY_FILE));
            } catch (Exception ignored) {
            }
        }
    }

    static String lastRuntimeStatus(Context context) {
        try {
            File status = new File(context.getFilesDir(), "vulkan-driver-status.json");
            if (!status.isFile() || status.length() <= 0L
                    || status.length() > MAX_METADATA_BYTES) {
                return "";
            }
            return readFileTextLimited(status, MAX_METADATA_BYTES).trim();
        } catch (Exception ignored) {
            return "";
        }
    }

    private static JSONObject validateProbeStatus(DriverPackage candidate, String rawStatus)
            throws Exception {
        if (rawStatus == null || rawStatus.isEmpty()
                || rawStatus.length() > MAX_METADATA_BYTES) {
            throw new IOException("Driver probe returned no valid status");
        }
        JSONObject status = new JSONObject(rawStatus);
        String expectedPath = candidate.library.getCanonicalPath();
        String requestedPath = new File(status.optString("requestedPath", ""))
                .getCanonicalPath();
        String effectivePath = new File(status.optString("path", ""))
                .getCanonicalPath();
        String expectedSha = candidate.sha256.toLowerCase(Locale.ROOT);
        String requestedSha = status.optString("requestedSha256", "")
                .toLowerCase(Locale.ROOT);
        String effectiveSha = status.optString("sha256", "")
                .toLowerCase(Locale.ROOT);
        String identity = (status.optString("driverName", "") + " "
                + status.optString("driverInfo", "")).toLowerCase(Locale.ROOT);
        boolean accepted = "turnip".equals(status.optString("requested"))
                && "turnip".equals(status.optString("effective"))
                && "none".equals(status.optString("fallbackReason"))
                && status.optBoolean("selectionLocked", false)
                && status.optBoolean("mappingVerified", false)
                && status.optBoolean("logicalDeviceProbePassed", false)
                && "imported".equals(status.optString("requestedPackageSource"))
                && "imported".equals(status.optString("packageSource"))
                && expectedPath.equals(requestedPath)
                && expectedPath.equals(effectivePath)
                && expectedSha.equals(requestedSha)
                && expectedSha.equals(effectiveSha)
                && (identity.contains("mesa") || identity.contains("turnip"));
        if (!accepted) {
            String reason = safeIdentity(status.optString("fallbackReason"),
                    "probe-evidence-mismatch");
            throw new IOException("Driver live probe rejected the package: " + reason);
        }
        if (!expectedSha.equals(sha256(candidate.library))) {
            throw new IOException("Driver changed after the live probe");
        }
        HardwareIdentity probedHardware = hardwareIdentityFromProbeStatus(status);
        if (probedHardware == null) {
            throw new IOException(
                    "Driver live probe did not identify supported Adreno hardware");
        }
        if (candidate.hardwareIdentity != null
                && !candidate.hardwareIdentity.matches(probedHardware)) {
            throw new IOException(
                    "Driver live probe hardware differs from its verification record");
        }
        putHardwareIdentity(status, probedHardware);
        return status;
    }

    private static void rollbackAfterFailures(File root, JSONObject failed, String message)
            throws Exception {
        File activeFile = new File(root, ACTIVE_FILE);
        deleteIfPresent(activeFile);
        JSONObject notice = new JSONObject();
        notice.put("schema", "lsx4.turnip-driver-recovery.v1");
        notice.put("message", message);
        notice.put("failedSha256", failed.optString("sha256", ""));
        notice.put("restored", "system");
        notice.put("createdAtMs", System.currentTimeMillis());
        writeJsonAtomic(root, new File(root, RECOVERY_FILE), notice);
    }

    private static JSONObject rollbackSnapshot(JSONObject active) throws Exception {
        JSONObject snapshot = new JSONObject(active.toString());
        snapshot.remove("previous");
        snapshot.put("consecutiveFailures", 0);
        snapshot.put("launchPending", false);
        return snapshot;
    }

    static String sha256(File file) throws Exception {
        MessageDigest digest = MessageDigest.getInstance("SHA-256");
        byte[] buffer = new byte[128 * 1024];
        try (InputStream input = new FileInputStream(file)) {
            for (int count = input.read(buffer); count >= 0; count = input.read(buffer)) {
                if (count > 0) {
                    digest.update(buffer, 0, count);
                }
            }
        }
        StringBuilder value = new StringBuilder(64);
        for (byte item : digest.digest()) {
            value.append(String.format(Locale.ROOT, "%02x", item & 0xff));
        }
        return value.toString();
    }

    private static DriverPackage readVerifiedImported(Context context, String requestedSha)
            throws Exception {
        DriverPackage candidate = readVerifiedImportedMetadata(context, requestedSha);
        if (candidate == null) {
            return null;
        }
        ElfVulkanDriverValidator.validate(candidate.library);
        JSONObject verification = readVerificationJson(candidate);
        JSONObject status = verification.optJSONObject("probeStatus");
        if (status == null) {
            return null;
        }
        validateProbeStatus(candidate, status.toString());
        return candidate;
    }

    private static DriverPackage readVerifiedImportedMetadata(
            Context context, String requestedSha) throws Exception {
        String sha = requestedSha == null ? ""
                : requestedSha.toLowerCase(Locale.ROOT);
        if (!sha.matches("[0-9a-f]{64}")) {
            return null;
        }
        File root = driverRoot(context).getCanonicalFile();
        File slots = new File(root, SLOTS_DIRECTORY).getCanonicalFile();
        File directory = new File(slots, sha).getCanonicalFile();
        if (!slots.equals(directory.getParentFile()) || !sha.equals(directory.getName())) {
            return null;
        }
        File verificationFile = new File(directory, VERIFIED_FILE);
        if (!verificationFile.isFile() || verificationFile.length() <= 0L
                || verificationFile.length() > MAX_METADATA_BYTES) {
            return null;
        }
        JSONObject verification = new JSONObject(
                readFileTextLimited(verificationFile, MAX_METADATA_BYTES));
        HardwareIdentity currentHardware = currentHardwareIdentity(context);
        HardwareIdentity verifiedHardware = hardwareIdentityFromMetadata(verification);
        if (!VERIFIED_SCHEMA.equals(verification.optString("schema"))
                || verification.optInt("probeContractVersion", -1)
                != PROBE_CONTRACT_VERSION
                || !sha.equals(verification.optString("slot", "").toLowerCase(Locale.ROOT))
                || !sha.equals(verification.optString("sha256", "").toLowerCase(Locale.ROOT))
                || !Build.FINGERPRINT.equals(
                verification.optString("deviceFingerprint", ""))
                || !Build.MODEL.equals(verification.optString("deviceModel", ""))
                || Build.VERSION.SDK_INT != verification.optInt("sdk", -1)
                || currentHardware == null
                || !currentHardware.matches(verifiedHardware)) {
            return null;
        }
        File packageFile = new File(directory, "package.json");
        if (!packageFile.isFile() || packageFile.length() <= 0L
                || packageFile.length() > MAX_METADATA_BYTES) {
            return null;
        }
        JSONObject packageMetadata = new JSONObject(
                readFileTextLimited(packageFile, MAX_METADATA_BYTES));
        if (!PACKAGE_SCHEMA.equals(packageMetadata.optString("schema"))
                || !sha.equals(packageMetadata.optString("sha256", "")
                .toLowerCase(Locale.ROOT))
                || !DRIVER_FILE_NAME.equals(packageMetadata.optString("driverFile", ""))) {
            return null;
        }
        File driver = new File(directory, DRIVER_FILE_NAME).getCanonicalFile();
        if (!directory.equals(driver.getParentFile()) || !driver.isFile()
                || driver.length() < 4096L
                || driver.length() != verification.optLong("librarySize", -1L)
                || driver.lastModified()
                != verification.optLong("libraryLastModifiedMs", -1L)
                || !isAarch64Elf(driver)) {
            return null;
        }
        DriverPackage candidate = new DriverPackage(directory, driver, sha,
                safeIdentity(verification.optString("label"), "Mesa Turnip"),
                safeIdentity(verification.optString("version"), ""), true,
                verifiedHardware);
        JSONObject status = verification.optJSONObject("probeStatus");
        if (status == null) {
            return null;
        }
        HardwareIdentity probedHardware = hardwareIdentityFromMetadata(status);
        String expectedPath = driver.getCanonicalPath();
        String requestedPath = new File(status.optString("requestedPath", ""))
                .getCanonicalPath();
        String effectivePath = new File(status.optString("path", ""))
                .getCanonicalPath();
        String identity = (status.optString("driverName", "") + " "
                + status.optString("driverInfo", "")).toLowerCase(Locale.ROOT);
        boolean recordedProbePassed = "turnip".equals(status.optString("requested"))
                && "turnip".equals(status.optString("effective"))
                && "none".equals(status.optString("fallbackReason"))
                && status.optBoolean("selectionLocked", false)
                && status.optBoolean("mappingVerified", false)
                && status.optBoolean("logicalDeviceProbePassed", false)
                && "imported".equals(status.optString("requestedPackageSource"))
                && "imported".equals(status.optString("packageSource"))
                && expectedPath.equals(requestedPath)
                && expectedPath.equals(effectivePath)
                && sha.equalsIgnoreCase(status.optString("requestedSha256", ""))
                && sha.equalsIgnoreCase(status.optString("sha256", ""))
                && verifiedHardware.matches(probedHardware)
                && (identity.contains("mesa") || identity.contains("turnip"));
        if (!recordedProbePassed) {
            return null;
        }
        return candidate;
    }

    private static JSONObject readVerificationJson(DriverPackage driver) throws Exception {
        File verificationFile = new File(driver.directory, VERIFIED_FILE);
        if (!verificationFile.isFile() || verificationFile.length() <= 0L
                || verificationFile.length() > MAX_METADATA_BYTES) {
            throw new IOException("Imported Vulkan driver has no successful probe record");
        }
        JSONObject verification = new JSONObject(
                readFileTextLimited(verificationFile, MAX_METADATA_BYTES));
        HardwareIdentity verifiedHardware = hardwareIdentityFromMetadata(verification);
        if (!VERIFIED_SCHEMA.equals(verification.optString("schema"))
                || !driver.sha256.equalsIgnoreCase(
                verification.optString("sha256", ""))
                || verifiedHardware == null
                || (driver.hardwareIdentity != null
                && !driver.hardwareIdentity.matches(verifiedHardware))) {
            throw new IOException("Imported Vulkan driver probe record is invalid");
        }
        return verification;
    }

    private static void activateSelectedImported(Context context, DriverPackage requested,
                                                 boolean explicitReselection)
            throws Exception {
        DriverPackage driver = readVerifiedImported(context, requested.sha256);
        if (driver == null) {
            throw new IOException("Imported Vulkan driver is not verified on this device");
        }
        File root = driverRoot(context);
        JSONObject verification = readVerificationJson(driver);
        JSONObject probeStatus = verification.optJSONObject("probeStatus");
        if (probeStatus == null) {
            throw new IOException("Imported Vulkan driver probe evidence is missing");
        }
        JSONObject status = validateProbeStatus(driver, probeStatus.toString());
        HardwareIdentity driverHardware = driver.hardwareIdentity;
        HardwareIdentity currentHardware = requireCurrentHardwareIdentity(context);
        if (driverHardware == null || !currentHardware.matches(driverHardware)) {
            throw new IOException(
                    "Imported Vulkan driver is verified for different hardware");
        }
        JSONObject current = readActiveJson(root);
        HardwareIdentity activeHardware = current == null ? null
                : hardwareIdentityFromMetadata(current);
        if (!explicitReselection && current != null && driver.sha256.equalsIgnoreCase(
                current.optString("sha256", ""))
                && driverHardware.matches(activeHardware)) {
            return;
        }
        JSONObject previous = current == null || !driverHardware.matches(activeHardware)
                ? null : rollbackSnapshot(current);
        JSONObject active = new JSONObject();
        active.put("schema", ACTIVE_SCHEMA);
        active.put("slot", driver.sha256);
        active.put("sha256", driver.sha256);
        active.put("label", driver.label);
        active.put("version", driver.version);
        active.put("driverName", status.optString("driverName", driver.label));
        active.put("driverInfo", status.optString("driverInfo", driver.version));
        active.put("driverVersion", status.optString("driverVersion", ""));
        putHardwareIdentity(active, driverHardware);
        active.put("probeStatus", status);
        active.put("probeScope", "identity-loadability-no-screen-present");
        active.put("screenPresentVerified", false);
        active.put("consecutiveFailures", 0);
        active.put("launchPending", false);
        active.put("activatedAtMs", System.currentTimeMillis());
        if (previous != null) {
            active.put("previous", previous);
        }
        writeJsonAtomic(root, new File(root, ACTIVE_FILE), active);
        JSONObject persisted = readActiveJson(root);
        if (explicitReselection && (persisted == null
                || persisted.optBoolean("launchPending", true)
                || persisted.optInt("consecutiveFailures", -1) != 0)) {
            throw new IOException(
                    "Explicit imported Vulkan driver selection was not re-armed");
        }
        DriverPackage readBack = readActiveImported(context);
        if (readBack == null || !driver.sha256.equalsIgnoreCase(readBack.sha256)) {
            throw new IOException("Selected imported Vulkan driver was not activated");
        }
    }

    private static DriverPackage readActiveImported(Context context) throws Exception {
        File root = driverRoot(context);
        JSONObject json = readActiveJson(root);
        if (json == null) {
            return null;
        }
        String slot = json.optString("slot", "").toLowerCase(Locale.ROOT);
        String expectedSha = json.optString("sha256", "").toLowerCase(Locale.ROOT);
        if (!slot.matches("[0-9a-f]{64}") || !slot.equals(expectedSha)) {
            return null;
        }
        HardwareIdentity currentHardware = currentHardwareIdentity(context);
        HardwareIdentity activeHardware = hardwareIdentityFromMetadata(json);
        if (currentHardware == null || !currentHardware.matches(activeHardware)) {
            return null;
        }
        File slots = new File(root, SLOTS_DIRECTORY).getCanonicalFile();
        File directory = new File(slots, slot).getCanonicalFile();
        if (!isInside(slots, directory)) {
            return null;
        }
        DriverPackage verified = readVerifiedImportedMetadata(context, expectedSha);
        return verified != null && activeHardware.matches(verified.hardwareIdentity)
                ? verified : null;
    }

    private static JSONObject readActiveJson(File root) throws Exception {
        File active = new File(root, ACTIVE_FILE);
        if (!active.isFile() || active.length() <= 0L
                || active.length() > MAX_METADATA_BYTES) {
            return null;
        }
        JSONObject json = new JSONObject(readFileTextLimited(active, MAX_METADATA_BYTES));
        return ACTIVE_SCHEMA.equals(json.optString("schema")) ? json : null;
    }

    private static DriverPackage readSlot(File directory, String expectedSha,
                                          String label, String version) throws Exception {
        File driver = new File(directory, DRIVER_FILE_NAME);
        if (!driver.isFile() || driver.length() < 4096L || !isAarch64Elf(driver)) {
            return null;
        }
        String actualSha = sha256(driver);
        if (!actualSha.equalsIgnoreCase(expectedSha)) {
            return null;
        }
        return new DriverPackage(directory, driver, actualSha, label, version, true);
    }

    private static int driverPriority(String name) {
        if (DRIVER_FILE_NAME.equals(name)) {
            return 0;
        }
        if (name.matches("vulkan\\.ad[^/]*\\.so")) {
            return 1;
        }
        if ("libvulkan_freedreno.so".equals(name)) {
            return 2;
        }
        if ("libvulkan_turnip.so".equals(name)) {
            return 3;
        }
        if (name.endsWith(".so") && name.contains("vulkan")
                && (name.contains("turnip") || name.contains("freedreno")
                || name.contains("adreno"))) {
            return 4;
        }
        return -1;
    }

    private static boolean zipEntryIsAarch64Elf(ZipFile archive, ZipEntry entry)
            throws IOException {
        if (entry.getSize() > MAX_LIBRARY_BYTES) {
            throw new IOException("Driver library exceeds the size limit");
        }
        byte[] header = new byte[20];
        try (InputStream input = archive.getInputStream(entry)) {
            return readFully(input, header) && isAarch64ElfHeader(header);
        }
    }

    private static boolean isAarch64Elf(File file) throws IOException {
        byte[] header = new byte[20];
        try (InputStream input = new FileInputStream(file)) {
            return readFully(input, header) && isAarch64ElfHeader(header);
        }
    }

    private static boolean isAarch64ElfHeader(byte[] header) {
        return header.length >= 20
                && (header[0] & 0xff) == 0x7f
                && header[1] == 'E' && header[2] == 'L' && header[3] == 'F'
                && header[4] == 2 && header[5] == 1
                && (header[16] & 0xff) == 3 && header[17] == 0
                && (header[18] & 0xff) == 0xb7 && header[19] == 0;
    }

    private static boolean readFully(InputStream input, byte[] target) throws IOException {
        int offset = 0;
        while (offset < target.length) {
            int count = input.read(target, offset, target.length - offset);
            if (count < 0) {
                return false;
            }
            offset += count;
        }
        return true;
    }

    private static boolean containsAnyAscii(File file, String... values) throws IOException {
        byte[][] needles = new byte[values.length][];
        for (int i = 0; i < values.length; ++i) {
            needles[i] = values[i].getBytes(StandardCharsets.US_ASCII);
        }
        int[] matched = new int[needles.length];
        byte[] buffer = new byte[128 * 1024];
        try (InputStream input = new FileInputStream(file)) {
            for (int count = input.read(buffer); count >= 0; count = input.read(buffer)) {
                for (int i = 0; i < count; ++i) {
                    byte value = buffer[i];
                    for (int n = 0; n < needles.length; ++n) {
                        if (value == needles[n][matched[n]]) {
                            if (++matched[n] == needles[n].length) {
                                return true;
                            }
                        } else {
                            matched[n] = value == needles[n][0] ? 1 : 0;
                        }
                    }
                }
            }
        }
        return false;
    }

    private static void validatePackageMetadata(String metadata, String driverName)
            throws IOException {
        if (metadata.trim().isEmpty()) {
            throw new IOException("Driver archive metadata is empty");
        }
        try {
            JSONObject json = new JSONObject(metadata);
            String vendor = json.optString("vendor", "").toLowerCase(Locale.ROOT);
            if (!vendor.isEmpty() && !vendor.contains("mesa")
                    && !vendor.contains("turnip") && !vendor.contains("freedreno")) {
                throw new IOException("Driver archive metadata is not for Mesa Turnip");
            }
            String abi = firstValue(json, "abi", "driverAbi").toLowerCase(Locale.ROOT);
            if (abi.contains("glibc") || abi.contains("linux-")) {
                throw new IOException("Linux/glibc driver archives are not supported");
            }
            int minimumApi = json.optInt("minApi", 0);
            if (minimumApi > Build.VERSION.SDK_INT) {
                throw new IOException("Driver requires Android API " + minimumApi);
            }
            String declaredLibrary = json.optString("libraryName", "");
            if (!declaredLibrary.isEmpty()
                    && (!declaredLibrary.equals(archiveBaseName(declaredLibrary))
                    || !declaredLibrary.equalsIgnoreCase(driverName))) {
                throw new IOException("Driver metadata names a different library");
            }
        } catch (IOException error) {
            throw error;
        } catch (Exception error) {
            throw new IOException("Invalid driver archive metadata", error);
        }
    }

    private static void copyUriLimited(Context context, Uri source, File destination,
                                       long maximum) throws IOException {
        long total = 0L;
        try (InputStream input = context.getContentResolver().openInputStream(source);
             FileOutputStream output = new FileOutputStream(destination)) {
            if (input == null) {
                throw new IOException("Could not open the selected archive");
            }
            byte[] buffer = new byte[256 * 1024];
            for (int count = input.read(buffer); count >= 0; count = input.read(buffer)) {
                if (count == 0) {
                    continue;
                }
                if (total > maximum - count) {
                    throw new IOException("Driver archive exceeds the size limit");
                }
                output.write(buffer, 0, count);
                total += count;
            }
            output.getFD().sync();
        } catch (IOException error) {
            destination.delete();
            throw error;
        }
        if (total == 0L) {
            destination.delete();
            throw new IOException("Selected driver archive is empty");
        }
    }

    private static void copyZipEntryLimited(ZipFile archive, ZipEntry entry, File destination,
                                            long[] expanded) throws IOException {
        long fileBytes = 0L;
        try (InputStream input = archive.getInputStream(entry);
             FileOutputStream output = new FileOutputStream(destination)) {
            byte[] buffer = new byte[256 * 1024];
            for (int count = input.read(buffer); count >= 0; count = input.read(buffer)) {
                if (count == 0) {
                    continue;
                }
                if (fileBytes > MAX_LIBRARY_BYTES - count
                        || expanded[0] > MAX_EXPANDED_BYTES - count) {
                    throw new IOException("Expanded driver archive exceeds the size limit");
                }
                output.write(buffer, 0, count);
                fileBytes += count;
                expanded[0] += count;
            }
            output.getFD().sync();
        }
        if (fileBytes == 0L) {
            throw new IOException("Driver archive contains an empty library");
        }
    }

    private static String readZipTextLimited(ZipFile archive, ZipEntry entry, int maximum)
            throws IOException {
        try (InputStream input = archive.getInputStream(entry)) {
            return readStreamTextLimited(input, maximum);
        }
    }

    private static String readFileTextLimited(File file, int maximum) throws IOException {
        try (InputStream input = new FileInputStream(file)) {
            return readStreamTextLimited(input, maximum);
        }
    }

    private static String readStreamTextLimited(InputStream input, int maximum)
            throws IOException {
        ByteArrayOutputStream output = new ByteArrayOutputStream();
        byte[] buffer = new byte[4096];
        int total = 0;
        for (int count = input.read(buffer); count >= 0; count = input.read(buffer)) {
            if (count == 0) {
                continue;
            }
            if (total > maximum - count) {
                throw new IOException("Driver metadata exceeds the size limit");
            }
            output.write(buffer, 0, count);
            total += count;
        }
        return new String(output.toByteArray(), StandardCharsets.UTF_8);
    }

    private static String[] driverIdentity(String metadata, String archiveName) {
        String name = "";
        String version = "";
        if (!metadata.trim().isEmpty()) {
            try {
                JSONObject json = new JSONObject(metadata);
                name = firstValue(json, "name", "driverName", "description");
                version = firstValue(json, "driverVersion", "versionName", "version");
            } catch (Exception ignored) {
            }
        }
        if (name.isEmpty()) {
            name = safeIdentity(archiveName, "Imported Mesa Turnip");
            int dot = name.toLowerCase(Locale.ROOT).lastIndexOf(".zip");
            if (dot > 0) {
                name = name.substring(0, dot);
            }
        }
        return new String[]{safeIdentity(name, "Imported Mesa Turnip"),
                safeIdentity(version, "")};
    }

    private static String firstValue(JSONObject json, String... keys) {
        for (String key : keys) {
            String value = safeIdentity(json.optString(key), "");
            if (!value.isEmpty()) {
                return value;
            }
        }
        return "";
    }

    private static String safeIdentity(String value, String fallback) {
        if (value == null) {
            return fallback;
        }
        StringBuilder clean = new StringBuilder();
        for (int i = 0; i < value.length() && clean.length() < 96; ++i) {
            char character = value.charAt(i);
            if (character >= 0x20 && character != 0x7f) {
                clean.append(character);
            }
        }
        String result = clean.toString().trim();
        return result.isEmpty() ? fallback : result;
    }

    private static boolean isSafeLibraryName(String name) {
        return name.length() <= 128 && name.matches("[A-Za-z0-9._+\\-]+\\.so");
    }

    private static void validateArchivePath(String name, boolean directory) throws IOException {
        if (name == null || name.isEmpty() || name.length() > 512
                || name.startsWith("/") || name.startsWith("\\")
                || name.indexOf('\\') >= 0 || name.indexOf('\0') >= 0
                || name.indexOf(':') >= 0) {
            throw new IOException("Unsafe path in driver archive");
        }
        String[] components = name.split("/", -1);
        for (int index = 0; index < components.length; ++index) {
            String component = components[index];
            boolean permittedDirectoryTerminator = directory
                    && index == components.length - 1 && component.isEmpty();
            if ((!permittedDirectoryTerminator && component.isEmpty())
                    || "..".equals(component) || ".".equals(component)) {
                throw new IOException("Path traversal in driver archive");
            }
        }
    }

    private static boolean isMacOsJunk(String path) {
        String lower = path.toLowerCase(Locale.ROOT);
        String base = archiveBaseName(lower);
        return lower.startsWith("__macosx/") || base.startsWith("._")
                || ".ds_store".equals(base);
    }

    private static String archiveBaseName(String path) {
        int slash = path.lastIndexOf('/');
        return slash < 0 ? path : path.substring(slash + 1);
    }

    private static File driverRoot(Context context) {
        return new File(context.getFilesDir(), ROOT_DIRECTORY);
    }

    private static void ensureDirectory(File directory) throws IOException {
        if (!directory.isDirectory() && !directory.mkdirs()) {
            throw new IOException("Could not create " + directory.getName());
        }
    }

    private static void deleteIfPresent(File file) throws IOException {
        if (file.exists() && !file.delete()) {
            throw new IOException("Could not remove " + file.getName());
        }
    }

    private static void writeJsonSynced(File destination, JSONObject json) throws IOException {
        try (FileOutputStream output = new FileOutputStream(destination)) {
            output.write((json.toString() + "\n").getBytes(StandardCharsets.UTF_8));
            output.getFD().sync();
        }
    }

    private static void writeJsonAtomic(File root, File destination, JSONObject json)
            throws IOException {
        File temporary = new File(root, destination.getName() + ".tmp");
        writeJsonSynced(temporary, json);
        try {
            atomicMove(temporary, destination);
        } finally {
            temporary.delete();
        }
    }

    private static void atomicMove(File source, File destination) throws IOException {
        try {
            Files.move(source.toPath(), destination.toPath(),
                    StandardCopyOption.ATOMIC_MOVE, StandardCopyOption.REPLACE_EXISTING);
        } catch (AtomicMoveNotSupportedException ignored) {
            Files.move(source.toPath(), destination.toPath(),
                    StandardCopyOption.REPLACE_EXISTING);
        }
    }

    private static void deleteTree(File root, File target) throws IOException {
        File canonicalRoot = root.getCanonicalFile();
        File canonicalTarget = target.getCanonicalFile();
        if (!isInside(canonicalRoot, canonicalTarget)) {
            throw new IOException("Refusing to clean a path outside the driver store");
        }
        File[] children = canonicalTarget.listFiles();
        if (children != null) {
            for (File child : children) {
                deleteTree(canonicalRoot, child);
            }
        }
        if (canonicalTarget.exists() && !canonicalTarget.delete()) {
            throw new IOException("Could not clean the driver staging directory");
        }
    }

    private static boolean isInside(File root, File target) {
        String rootPath = root.getAbsolutePath();
        String targetPath = target.getAbsolutePath();
        return !rootPath.equals(targetPath)
                && targetPath.startsWith(rootPath + File.separator);
    }
}
