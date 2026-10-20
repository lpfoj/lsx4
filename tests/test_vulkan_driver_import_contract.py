from pathlib import Path
import re
import unittest
import xml.etree.ElementTree as ElementTree


ROOT = Path(__file__).resolve().parents[1]


def source(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


MANIFEST = source("android-app/app/src/main/AndroidManifest.xml")
APPLICATION = source(
    "android-app/app/src/main/java/app/lsx4/android/Lsx4Application.java"
)
SETTINGS = source(
    "android-app/app/src/main/java/app/lsx4/android/SettingsActivity.java"
)
MAIN_ACTIVITY = source(
    "android-app/app/src/main/java/app/lsx4/android/MainActivity.java"
)
DRIVER_MANAGER = source(
    "android-app/app/src/main/java/app/lsx4/android/VulkanDriverManager.java"
)
ELF_VALIDATOR = source(
    "android-app/app/src/main/java/app/lsx4/android/ElfVulkanDriverValidator.java"
)
PROBE = source(
    "android-app/app/src/main/java/app/lsx4/android/VulkanDriverProbe.java"
)
PROBE_ACTIVITY = source(
    "android-app/app/src/main/java/app/lsx4/android/VulkanDriverProbeActivity.java"
)
RUNTIME_BRIDGE_JAVA = source(
    "android-app/app/src/main/java/app/lsx4/android/RuntimeBridge.java"
)
RUNTIME_BRIDGE_CPP = source("android-app/app/src/main/cpp/runtime_bridge.cpp")
NATIVE_RUNTIME_API = source("src/executor/native_runtime_api.cpp")
DRIVER_AXIS = source(
    "funnel-arm/src/video_core/renderer_vulkan/vk_driver_axis.cpp"
)
DEFAULT_STRINGS = source("android-app/app/src/main/res/values/strings.xml")
RU_STRINGS = source("android-app/app/src/main/res/values-ru/strings.xml")
IMPORT_SPEC = source("docs/VULKAN_DRIVER_IMPORT_SPEC_2026-08-21.md")


def braced_block(text: str, marker: str) -> str:
    """Return a source block while ignoring braces in strings and comments."""
    start = text.index(marker)
    cursor = text.index("{", start)
    depth = 0
    state = "code"
    block_start = cursor
    while cursor < len(text):
        char = text[cursor]
        following = text[cursor + 1] if cursor + 1 < len(text) else ""
        if state == "code":
            if char == "/" and following == "/":
                state = "line-comment"
                cursor += 2
                continue
            if char == "/" and following == "*":
                state = "block-comment"
                cursor += 2
                continue
            if char == '"':
                state = "string"
            elif char == "'":
                state = "character"
            elif char == "{":
                depth += 1
            elif char == "}":
                depth -= 1
                if depth == 0:
                    return text[start : cursor + 1]
        elif state == "line-comment":
            if char == "\n":
                state = "code"
        elif state == "block-comment":
            if char == "*" and following == "/":
                state = "code"
                cursor += 2
                continue
        elif state in ("string", "character"):
            if char == "\\":
                cursor += 2
                continue
            if (state == "string" and char == '"') or (
                state == "character" and char == "'"
            ):
                state = "code"
        cursor += 1
    raise AssertionError(
        f"unterminated block after {marker!r} (opening brace at {block_start})"
    )


class VulkanDriverImportContractTest(unittest.TestCase):
    def test_import_probe_activation_and_recovery_contract(self) -> None:
        with self.subTest(contract="dynamic dropdown and stable selection migration"):
            selection = braced_block(DRIVER_MANAGER, "static String readSelectionId(")
            writer = braced_block(DRIVER_MANAGER, "static void selectDriver(")
            listing = braced_block(DRIVER_MANAGER, "static List<DriverEntry> listDrivers(")
            self.assertIn("Spinner vulkanDriverSpinner = spinner(", SETTINGS)
            self.assertIn("vulkanDriverLabels(vulkanDrivers, selectedDriver", SETTINGS)
            self.assertIn('label("+", 26', SETTINGS)
            self.assertIn("setMinWidth(dp(48))", SETTINGS)
            self.assertIn("setMinHeight(dp(48))", SETTINGS)
            self.assertIn("setContentDescription(", SETTINGS)
            self.assertIn('static final String SELECTION_SYSTEM = "system"', DRIVER_MANAGER)
            self.assertIn('static final String SELECTION_BUNDLED = "bundled"', DRIVER_MANAGER)
            self.assertIn(
                'static final String SELECTION_IMPORTED_PREFIX = "imported:"',
                DRIVER_MANAGER,
            )
            self.assertIn("current instanceof String", selection)
            self.assertNotIn("migrateActiveVerification", selection)
            self.assertIn("persistSelection(preferences, migrated)", selection)
            self.assertIn("activateSelectedImported(context, driver)", writer)
            self.assertIn("persistSelection(preferences, selected)", writer)
            self.assertIn("result.add(new DriverEntry(SELECTION_SYSTEM", listing)
            self.assertIn("result.add(new DriverEntry(SELECTION_BUNDLED", listing)
            self.assertIn("readVerifiedImportedMetadata(context, sha)", listing)
            self.assertIn("importedSelectionId(sha)", listing)
            driver_controls = SETTINGS[
                SETTINGS.index("boolean driverAxisSupported =") :
                SETTINGS.index("root.addView(preferenceCheck(R.string.optimization_arm_gpu_fast_path")
            ]
            self.assertNotIn("CheckBox", driver_controls)
            self.assertNotIn("actionCard(", driver_controls)
            self.assertNotIn("importedStatus", driver_controls)
            self.assertNotIn("useBundled", driver_controls)

        with self.subTest(contract="bundled choice is the exact D6 payload"):
            bundled = braced_block(DRIVER_MANAGER, "private static DriverPackage bundledDriver(")
            bundled_hardware = braced_block(
                DRIVER_MANAGER,
                "static boolean isBundledTurnipSupportedHardware(",
            )
            self.assertIn('static final String BUNDLED_TURNIP_VERSION = "26.2.1"',
                          DRIVER_MANAGER)
            self.assertIn(
                '"b56a4cec9af33af61ec068efb1d8673b54bb325635ca7a2300db77df82d752cd"',
                DRIVER_MANAGER,
            )
            bundled_order = [
                bundled.index("String bundledSha = sha256(bundled)"),
                bundled.index("BUNDLED_TURNIP_SHA256.equals(bundledSha)"),
                bundled.index("ElfVulkanDriverValidator.validate(bundled)"),
                bundled.index("new DriverPackage(nativeDir, bundled, bundledSha"),
            ]
            self.assertEqual(bundled_order, sorted(bundled_order))
            self.assertIn("ADRENO_840_VULKAN_DEVICE_ID", bundled_hardware)
            self.assertIn("ADRENO_750_VULKAN_DEVICE_ID", bundled_hardware)
            self.assertIn("QUALCOMM_VULKAN_VENDOR_ID", bundled_hardware)
            self.assertIn("isBundledTurnipSupportedHardware(", bundled)
            self.assertLess(
                bundled.index("isBundledTurnipSupportedHardware("),
                bundled.index("String bundledSha = sha256(bundled)"),
            )
            self.assertIn(
                '<string name="optimization_vulkan_driver_bundled_option">'
                'Mesa Turnip %1$s (built-in)</string>',
                DEFAULT_STRINGS,
            )
            self.assertIn(
                '<string name="optimization_vulkan_driver_bundled_option">'
                'Mesa Turnip %1$s (встроенный)</string>',
                RU_STRINGS,
            )
            self.assertNotIn("built-in, tested", DEFAULT_STRINGS)
            self.assertNotIn("встроенный, проверен", RU_STRINGS)
            self.assertIn("Adreno 750 и Adreno 840", IMPORT_SPEC)
            self.assertIn("не переносится на прочие Adreno", IMPORT_SPEC)

        with self.subTest(contract="only device-probed immutable slots are listed"):
            listing = braced_block(DRIVER_MANAGER, "static List<DriverEntry> listDrivers(")
            verified = braced_block(
                DRIVER_MANAGER, "private static DriverPackage readVerifiedImported("
            )
            verified_metadata = braced_block(
                DRIVER_MANAGER,
                "private static DriverPackage readVerifiedImportedMetadata(",
            )
            list_order = [
                listing.index("new DriverEntry(SELECTION_SYSTEM"),
                listing.index("new DriverEntry(SELECTION_BUNDLED"),
                listing.index("result.addAll(imported)"),
            ]
            self.assertEqual(list_order, sorted(list_order))
            self.assertIn("readVerifiedImportedMetadata(context, sha)", listing)
            self.assertNotIn("readSlot(", listing)
            self.assertIn(
                "private static final int PROBE_CONTRACT_VERSION = 3",
                DRIVER_MANAGER,
            )
            for evidence in (
                "new File(directory, VERIFIED_FILE)",
                "VERIFIED_SCHEMA.equals",
                'verification.optInt("probeContractVersion", -1)',
                "Build.FINGERPRINT.equals",
                "Build.MODEL.equals",
                "PACKAGE_SCHEMA.equals",
                'verification.optLong("librarySize", -1L)',
                'verification.optLong("libraryLastModifiedMs", -1L)',
                'status.optBoolean("logicalDeviceProbePassed", false)',
            ):
                self.assertIn(evidence, verified_metadata)
            for evidence in (
                "readVerifiedImportedMetadata(context, requestedSha)",
                "ElfVulkanDriverValidator.validate(candidate.library)",
                'verification.optJSONObject("probeStatus")',
                "validateProbeStatus(candidate, status.toString())",
            ):
                self.assertIn(evidence, verified)

        with self.subTest(contract="Turnip metadata is bound to raw Vulkan hardware"):
            current_hardware = braced_block(
                DRIVER_MANAGER, "private static HardwareIdentity currentHardwareIdentity("
            )
            probe_hardware = braced_block(
                DRIVER_MANAGER,
                "private static HardwareIdentity hardwareIdentityFromProbeStatus(",
            )
            register = braced_block(
                DRIVER_MANAGER, "static DriverPackage registerImported("
            )
            verified_metadata = braced_block(
                DRIVER_MANAGER,
                "private static DriverPackage readVerifiedImportedMetadata(",
            )
            activate = braced_block(
                DRIVER_MANAGER, "private static void activateSelectedImported("
            )
            active_reader = braced_block(
                DRIVER_MANAGER, "private static DriverPackage readActiveImported("
            )
            listing = braced_block(
                DRIVER_MANAGER, "static List<DriverEntry> listDrivers("
            )
            resolver = braced_block(
                DRIVER_MANAGER, "static DriverPackage resolveSelection("
            )
            bundled = braced_block(
                DRIVER_MANAGER, "private static DriverPackage bundledDriver("
            )
            importer = braced_block(
                DRIVER_MANAGER, "static ImportResult importZip("
            )
            validate_status = braced_block(
                DRIVER_MANAGER, "private static JSONObject validateProbeStatus("
            )

            for declaration in (
                "QUALCOMM_VULKAN_VENDOR_ID = 0x5143L",
                "ADRENO_750_VULKAN_DEVICE_ID = 0x43051401L",
                "ADRENO_840_VULKAN_DEVICE_ID = 0x44050a31L",
                'VULKAN_VENDOR_ID_FIELD = "vulkanVendorId"',
                'VULKAN_DEVICE_ID_FIELD = "vulkanDeviceId"',
                'VULKAN_HARDWARE_KEY_FIELD = "vulkanHardwareKey"',
                'new File("/sys/class/kgsl/kgsl-3d0/gpu_model")',
            ):
                self.assertIn(declaration, DRIVER_MANAGER)
            for mapping in (
                'normalized.matches("(?:turnip)?adreno(?:tm)?750(?:v[0-9]+)?")',
                'normalized.matches("(?:turnip)?adreno(?:tm)?840(?:v[0-9]+)?")',
                '"sm8650".equals(normalized)',
                '"sm8850".equals(normalized)',
            ):
                self.assertIn(mapping, DRIVER_MANAGER)
            gpu_signal = braced_block(
                DRIVER_MANAGER,
                "private static HardwareIdentity hardwareIdentityFromGpuSignal(",
            )
            gpu_patterns = re.findall(
                r'normalized\.matches\("([^"]+)"\)', gpu_signal
            )
            self.assertEqual(len(gpu_patterns), 2)
            for accepted_name in (
                "Adreno750v2",
                "Turnip Adreno (TM) 750",
                "Adreno (TM) 840",
                "Turnip Adreno 840",
            ):
                normalized_name = re.sub(
                    "[^a-z0-9]", "", accepted_name.lower()
                )
                self.assertTrue(any(
                    re.fullmatch(pattern, normalized_name)
                    for pattern in gpu_patterns
                ))
            for rejected_name in ("Mali-G715", "Adreno 830", "unknown"):
                normalized_name = re.sub(
                    "[^a-z0-9]", "", rejected_name.lower()
                )
                self.assertFalse(any(
                    re.fullmatch(pattern, normalized_name)
                    for pattern in gpu_patterns
                ))
            self.assertIn("readFileTextLimited(KGSL_GPU_MODEL_FILE, 256)",
                          current_hardware)
            self.assertIn("Build.SOC_MODEL", current_hardware)
            self.assertIn('"vendorId", "deviceId", "vendorID", "deviceID"',
                          probe_hardware)
            self.assertIn('status.optString("deviceName", "")', probe_hardware)

            registration_order = [
                register.index("requireCurrentHardwareIdentity(context)"),
                register.index("validateProbeStatus(verifiedSlot, probeStatus)"),
                register.index("currentHardware.matches(probedHardware)"),
                register.index("putHardwareIdentity(verification, currentHardware)"),
                register.index('verification.put("probeStatus", status)'),
            ]
            self.assertEqual(registration_order, sorted(registration_order))
            self.assertIn("putHardwareIdentity(status, probedHardware)", validate_status)
            self.assertIn("candidate.hardwareIdentity.matches(probedHardware)",
                          validate_status)

            for evidence in (
                "hardwareIdentityFromMetadata(verification)",
                "currentHardwareIdentity(context)",
                "currentHardware.matches(verifiedHardware)",
                "verifiedHardware.matches(probedHardware)",
            ):
                self.assertIn(evidence, verified_metadata)
            self.assertIn("putHardwareIdentity(active, driverHardware)", activate)
            self.assertIn("driverHardware.matches(activeHardware)", activate)
            self.assertIn("hardwareIdentityFromMetadata(json)", active_reader)
            self.assertIn("currentHardware.matches(activeHardware)", active_reader)
            self.assertNotIn("migrateActiveVerification(", DRIVER_MANAGER)



            self.assertLess(
                listing.index("currentHardwareIdentity(context) == null"),
                listing.index("bundledDriver(context)"),
            )
            self.assertIn("currentHardwareIdentity(context) == null", resolver)
            self.assertIn("requireCurrentHardwareIdentity(context)", importer)
            self.assertIn("currentHardware.matches(bundledCache.hardwareIdentity)",
                          bundled)
            self.assertIn("currentHardware);", bundled)

        with self.subTest(contract="stable string selection crosses the settings provider"):
            provider = source("android-app/app/src/main/java/app/lsx4/android/SettingsProvider.java")
            process_prefs = source("android-app/app/src/main/java/app/lsx4/android/ProcessSafePreferences.java")
            prefs_for_game = braced_block(SETTINGS, "public static SharedPreferences prefsForGame(")
            apply_driver = braced_block(
                MAIN_ACTIVITY, "private void applyVulkanDriverAxisSelection()"
            )
            self.assertIn("VulkanDriverManager.PREF_DRIVER_SELECTION_ID", SETTINGS)
            self.assertIn("ProcessSafePreferences.get(context)", prefs_for_game)
            self.assertIn("GameProfileSettings.forGame(base, titleId)", prefs_for_game)
            self.assertIn("SettingsProvider.remote(storeContext)", process_prefs)
            pack = braced_block(provider, "private static Bundle pack(")
            unpack = braced_block(provider, "private static Map<String, Object> unpack(")
            persist = braced_block(provider, "private static void put(")
            self.assertIn("value instanceof String", pack)
            self.assertIn("result.putString(key, (String) value)", pack)
            self.assertIn("value instanceof String", unpack)
            self.assertIn("result.put(key, value)", unpack)
            self.assertIn("editor.putString(key, (String) value)", persist)
            self.assertIn("change.values.put(key, value)",
                          braced_block(process_prefs, "public Editor putString("))
            self.assertIn("SharedPreferences preferences = gamePreferences()", apply_driver)
            self.assertIn("String requestedSelection", apply_driver)
            self.assertIn("readSelectionId(this, preferences)", apply_driver)
            self.assertIn("resolveSelection(this, requestedSelection)", apply_driver)
            self.assertNotIn("resolveTurnipDriver(this)", apply_driver)
            self.assertNotIn("readSelection(preferences)", apply_driver)
            live = braced_block(MAIN_ACTIVITY, "private void applyLiveSetting(")
            self.assertIn("GraphicsBackend.selected(this, preferences).writeLaunchConfiguration(this)", live)
            self.assertNotIn("GraphicsBackend.selected(this).", live)
            self.assertIn("GraphicsBackend.selected(this, appSettings).writeLaunchConfiguration(this)",
                          braced_block(MAIN_ACTIVITY, "protected void onCreate("))
            resolver = braced_block(DRIVER_MANAGER, "static DriverPackage resolveSelection(")
            self.assertIn("readVerifiedImported(context", resolver)
            self.assertNotIn("readVerifiedImportedMetadata(context", resolver)

        with self.subTest(contract="isolated non-exported software probe process"):
            android = "{http://schemas.android.com/apk/res/android}"
            manifest = ElementTree.fromstring(MANIFEST)
            probe_activity = next(
                activity
                for activity in manifest.findall(".//activity")
                if activity.get(android + "name")
                == "app.lsx4.android.VulkanDriverProbeActivity"
            )
            self.assertEqual(probe_activity.get(android + "process"), ":driverprobe")
            self.assertEqual(probe_activity.get(android + "exported"), "false")
            self.assertEqual(
                probe_activity.get(android + "hardwareAccelerated"), "false"
            )
            app_create = braced_block(APPLICATION, "public void onCreate()")
            self.assertLess(
                app_create.index("if (isDriverProbeProcess())"),
                app_create.index("RemoteDiagnostics.initialize(this)"),
            )
            self.assertIn("return;", braced_block(app_create, "if (isDriverProbeProcess())"))
            self.assertIn(
                'return (getPackageName() + ":driverprobe").equals(processName)',
                APPLICATION,
            )

        with self.subTest(contract="import registers a verified slot without selecting it"):
            importer = braced_block(DRIVER_MANAGER, "static ImportResult importZip(")
            register = braced_block(
                DRIVER_MANAGER, "static DriverPackage registerImported("
            )
            self.assertNotIn("ACTIVE_FILE", importer)
            self.assertNotIn("writeJsonAtomic", importer)
            registration_order = [
                register.index("validateProbeStatus(verifiedSlot, probeStatus)"),
                register.index("verification.put(\"probeStatus\", status)"),
                register.index("new File(verifiedSlot.directory, VERIFIED_FILE)"),
                register.index("readVerifiedImported("),
            ]
            self.assertEqual(registration_order, sorted(registration_order))
            self.assertNotIn("PREF_DRIVER_SELECTION", register)
            self.assertNotIn("ACTIVE_FILE", register)
            self.assertIn("Registered driver could not be read back", register)
            discard = braced_block(
                DRIVER_MANAGER, "static void discardInactiveImport("
            )
            finish = braced_block(
                SETTINGS, "private void finishTurnipDriverImport("
            )
            for block in (importer, register, discard, finish):
                self.assertNotIn("persistSelection(", block)
                self.assertNotIn("selectDriver(", block)
            self.assertIn("registerImported(", finish)
            self.assertIn("discardInactiveImport(appContext, staged)", finish)
            self.assertNotIn("selectVulkanDriver(", finish)

        with self.subTest(contract="unavailable selection fails closed with exact recovery"):
            selection = braced_block(DRIVER_MANAGER, "static String readSelectionId(")
            recovery = braced_block(
                DRIVER_MANAGER, "static String recoverUnavailableSelection("
            )
            record = braced_block(
                DRIVER_MANAGER, "private static String recordUnavailableSelection("
            )
            consume = braced_block(
                DRIVER_MANAGER, "static String consumeRecoveryNotice("
            )
            apply_driver = braced_block(
                MAIN_ACTIVITY, "private void applyVulkanDriverAxisSelection()"
            )
            self.assertIn("recoverUnavailableSelection(context, preferences, rawSelection",
                          selection)
            recovery_order = [
                recovery.index("recordUnavailableSelection(context, selectionId, reason)"),
                recovery.index("deleteIfPresent(new File(driverRoot(context), ACTIVE_FILE))"),
                recovery.index("persistSelection(preferences, SELECTION_SYSTEM)"),
            ]
            self.assertEqual(recovery_order, sorted(recovery_order))
            for evidence in (
                'notice.put("failedSelectionId", selected)',
                'notice.put("failedSha256"',
                'notice.put("reason"',
                'notice.put("restored", "system")',
                "new File(root, RECOVERY_FILE)",
            ):
                self.assertIn(evidence, record)
            main_recovery = apply_driver.index(
                "VulkanDriverManager.recoverUnavailableSelection("
            )
            self.assertLess(main_recovery,
                            apply_driver.index("requestedTurnip = false", main_recovery))
            self.assertIn("this, preferences, requestedSelection", apply_driver)
            self.assertLess(
                consume.index("persistSelection(preferences, SELECTION_SYSTEM)"),
                consume.index("deleteIfPresent(new File(driverRoot(context), RECOVERY_FILE))"),
            )

        with self.subTest(contract="one root DSO, root metadata, and archive path guards"):
            importer = braced_block(DRIVER_MANAGER, "static ImportResult importZip(")
            path_guard = braced_block(
                DRIVER_MANAGER, "private static void validateArchivePath("
            )
            required_import_guards = (
                "validateArchivePath(entry.getName(), entry.isDirectory())",
                "entryNames.add(nameKey)",
                "entry.isDirectory()",
                "entry.getName().indexOf('/') >= 0",
                '"meta.json".equals(lower)',
                "metadataEntry != null",
                'lower.endsWith(".so")',
                "if (driver != null)",
                "Driver archive must contain exactly one library",
                "metadataEntry == null || driver == null",
                "root meta.json and exactly one library",
                "validatePackageMetadata(metadata, driver.baseName)",
                "isSafeLibraryName(baseName)",
                "zipEntryIsAarch64Elf(archive, entry)",
            )
            for guard in required_import_guards:
                self.assertIn(guard, importer)
            for guard in (
                'name.startsWith("/")',
                'name.startsWith("\\\\")',
                "name.indexOf('\\\\') >= 0",
                "name.indexOf('\\0') >= 0",
                "name.indexOf(':') >= 0",
                'name.split("/", -1)',
                '"..".equals(component)',
                '".".equals(component)',
            ):
                self.assertIn(guard, path_guard)

        with self.subTest(contract="ELF dynsym HMI and Android HAL header parser"):
            hmi = braced_block(ELF_VALIDATOR, "private static void validateHmi(")
            self.assertIn("private static final int SHT_DYNSYM = 11", ELF_VALIDATOR)
            self.assertIn("private static final int PT_LOAD = 1", ELF_VALIDATOR)
            self.assertIn(
                "private static final long HARDWARE_MODULE_TAG = 0x48574d54L",
                ELF_VALIDATOR,
            )
            for evidence in (
                "if (symbols.type != SHT_DYNSYM)",
                '!"HMI".equals(readString(input, strings, nameOffset))',
                "int binding = info >>> 4",
                "int type = info & 0x0f",
                "int visibility = other & 0x03",
                "size < MIN_HMI_SIZE",
                "if (hmiCount != 1)",
                "for (LoadSegment load : loads)",
                "hmiVirtualAddress - load.virtualAddress",
                "tag != HARDWARE_MODULE_TAG",
                "moduleApi != HWVULKAN_MODULE_API_VERSION_0_1",
                "halApi != 0x0000 && halApi != 0x0100",
            ):
                self.assertIn(evidence, hmi)

        with self.subTest(contract="v2 ABI keeps helper and imported-driver dirs split"):
            jni_setter = braced_block(
                RUNTIME_BRIDGE_CPP,
                "Java_app_lsx4_android_RuntimeBridge_setVulkanDriverAxis(",
            )
            native_v2 = braced_block(
                NATIVE_RUNTIME_API,
                'extern "C" int executor_lsx4_runtime_set_vulkan_driver_axis_v2(',
            )
            configure = braced_block(DRIVER_AXIS, "int Configure(")
            select = braced_block(DRIVER_AXIS, "PFN_vkGetInstanceProcAddr SelectGetInstanceProcAddr(")
            self.assertIn(
                "String nativeLibraryDir, String turnipLibraryDir",
                RUNTIME_BRIDGE_JAVA,
            )
            self.assertIn("if (setter_v2)", jni_setter)
            self.assertIn(
                "setter_v2(requested_turnip ? 1 : 0, native_dir.c_str(),",
                jni_setter,
            )
            self.assertIn("turnip_dir.c_str(), report.c_str(), sha.c_str())", jni_setter)
            self.assertIn("(!requested_turnip || turnip_dir == native_dir)", jni_setter)
            self.assertIn(
                "Configure(requested_turnip != 0, native_library_dir,",
                native_v2,
            )
            self.assertIn("turnip_library_dir, report_path, turnip_sha256)", native_v2)
            self.assertIn("state.native_library_dir = NormalizePath", configure)
            self.assertIn("state.turnip_library_dir = NormalizePath", configure)
            self.assertIn("std::string hook_dir = state.native_library_dir", select)
            self.assertIn("std::string driver_dir = state.turnip_library_dir", select)
            self.assertIn(
                "open_libvulkan(RTLD_NOW | RTLD_LOCAL, AdrenoToolsDriverCustom",
                select,
            )

        with self.subTest(contract="live probe requires effective mapping, path, and SHA"):
            live_probe = braced_block(PROBE_ACTIVITY, "private void runProbe()")
            validate_status = braced_block(
                DRIVER_MANAGER, "private static JSONObject validateProbeStatus("
            )
            refresh_status = braced_block(DRIVER_AXIS, "void RefreshStatusLocked()")
            probe_jni = braced_block(
                RUNTIME_BRIDGE_CPP,
                "Java_app_lsx4_android_RuntimeBridge_probeVulkanDriverAxis(",
            )
            probe_order = [
                live_probe.index("validateProbeTarget(this, directory, expectedSha)"),
                live_probe.index("RuntimeBridge.probeVulkanDriverAxis("),
                live_probe.index('"turnip".equals(status.optString("effective"))'),
            ]
            self.assertEqual(probe_order, sorted(probe_order))
            for criterion in (
                '"turnip".equals(status.optString("requested"))',
                '"turnip".equals(status.optString("effective"))',
                '"none".equals(status.optString("fallbackReason"))',
                'status.optBoolean("selectionLocked", false)',
                'status.optBoolean("mappingVerified", false)',
                'status.optBoolean("logicalDeviceProbePassed", false)',
                'identity.contains("mesa") || identity.contains("turnip")',
            ):
                self.assertIn(criterion, live_probe)
            for criterion in (
                '"imported".equals(status.optString("requestedPackageSource"))',
                '"imported".equals(status.optString("packageSource"))',
                "expectedPath.equals(requestedPath)",
                "expectedPath.equals(effectivePath)",
                "expectedSha.equals(requestedSha)",
                "expectedSha.equals(effectiveSha)",
                "expectedSha.equals(sha256(candidate.library))",
            ):
                self.assertIn(criterion, validate_status)
            for evidence in (
                '{"requestedPath", state.requested_turnip ? state.turnip_path',
                '{"requestedSha256", state.requested_turnip ? state.turnip_sha256',
                '{"path", effective_turnip ? state.turnip_path',
                '{"sha256", effective_turnip ? state.turnip_sha256',
                '{"mappingVerified", state.mapping_verified}',
                '{"logicalDeviceProbePassed", state.logical_device_probe_passed}',
            ):
                self.assertIn(evidence, refresh_status)
            self.assertIn('dlopen(runtime.c_str(), RTLD_NOW | RTLD_LOCAL)', probe_jni)
            self.assertIn(
                'dlsym(probe_runtime, "executor_lsx4_runtime_probe_vulkan_driver_axis_v2")',
                probe_jni,
            )

        with self.subTest(contract="logical-device probe is isolated from live renderer"):
            native_probe = braced_block(
                NATIVE_RUNTIME_API,
                'extern "C" const char* executor_lsx4_runtime_probe_vulkan_driver_axis_v2(',
            )
            preflight = braced_block(DRIVER_AXIS, "const char* PreflightLocked(")
            live_platform = source(
                "funnel-arm/src/video_core/renderer_vulkan/vk_platform.cpp"
            )
            self.assertIn(
                "SelectGetInstanceProcAddr(system_gipa, true)", native_probe
            )
            self.assertIn("if (disposable_probe)", preflight)
            self.assertGreater(
                preflight.index("if (disposable_probe)"),
                preflight.index('return "custom-driver-identity-mismatch"'),
            )
            self.assertIn("create_device(chosen, &device_create_info", preflight)
            self.assertIn("state.logical_device_probe_passed = true", preflight)
            self.assertIn(
                "SelectGetInstanceProcAddr(system_get_instance_proc_addr)",
                live_platform,
            )
            self.assertNotIn(
                "SelectGetInstanceProcAddr(system_get_instance_proc_addr, true)",
                live_platform,
            )

        with self.subTest(contract="crash journals and persisted launch marker"):
            probe_start = braced_block(PROBE, "static void start(Activity owner,")
            probe_on_create = braced_block(
                PROBE_ACTIVITY, "protected void onCreate(Bundle state)"
            )
            probe_complete = braced_block(PROBE_ACTIVITY, "private void complete(")
            begin_launch = braced_block(
                DRIVER_MANAGER, "static LaunchDecision beginGameLaunch("
            )
            self.assertLess(
                probe_on_create.index('writeJournal("started", started)'),
                probe_on_create.index("receiver.send(VulkanDriverProbe.RESULT_STARTED"),
            )
            self.assertLess(
                probe_start.index('readJournal(owner, probeId, "result")'),
                probe_start.index('readJournal(owner, probeId, "started")'),
            )
            self.assertIn('started && !alive ? "crash" : "timeout"', probe_start)
            self.assertIn('writeJournal("result", result)', probe_complete)
            marker_order = [
                begin_launch.index('active.put("launchPending", true)'),
                begin_launch.index(
                    "writeJsonAtomic(root, new File(root, ACTIVE_FILE), active)"
                ),
                begin_launch.index("JSONObject persisted = readActiveJson(root)"),
                begin_launch.index('persisted.optBoolean("launchPending", false)'),
            ]
            self.assertEqual(marker_order, sorted(marker_order))

        with self.subTest(contract="three failures rollback and first-present confirmation"):
            begin_launch = braced_block(
                DRIVER_MANAGER, "static LaunchDecision beginGameLaunch("
            )
            confirm_launch = braced_block(
                DRIVER_MANAGER, "static void confirmGameLaunch("
            )
            health_check = braced_block(
                MAIN_ACTIVITY,
                "private final Runnable vulkanDriverHealthCheck = new Runnable()",
            )
            self.assertIn("private static final int MAX_CONSECUTIVE_FAILURES = 3", DRIVER_MANAGER)
            self.assertIn('active.optBoolean("launchPending", false)', begin_launch)
            self.assertIn("failures++", begin_launch)
            self.assertIn("if (failures >= MAX_CONSECUTIVE_FAILURES)", begin_launch)
            self.assertIn("rollbackAfterFailures(root, active, message)", begin_launch)
            self.assertIn(
                "presents >= vulkanDriverHealthInitialPresentCount + 1L",
                health_check,
            )
            self.assertIn(
                "importedTurnipIsActuallyEffective(pendingVulkanDriverHealthSha)",
                health_check,
            )
            self.assertIn("VulkanDriverManager.confirmGameLaunch(", health_check)
            for confirmation in (
                'active.put("consecutiveFailures", 0)',
                'active.put("launchPending", false)',
                'active.put("screenPresentVerified", true)',
                'active.put("screenPresentEvidence", "runtime-present-counter")',
                "writeJsonAtomic(root, new File(root, ACTIVE_FILE), active)",
            ):
                self.assertIn(confirmation, confirm_launch)

        with self.subTest(contract="dropdown stores a stable ID, never an adapter position"):
            ui_start = SETTINGS.index("boolean driverAxisSupported =")
            ui_end = SETTINGS.index(
                "root.addView(preferenceCheck(R.string.optimization_arm_gpu_fast_path",
                ui_start,
            )
            driver_ui = SETTINGS[ui_start:ui_end]
            listener_start = driver_ui.index(
                "vulkanDriverSpinner.setOnItemSelectedListener"
            )
            listener = driver_ui[listener_start:]
            self.assertIn(
                "if (!driverAxisSupported || !recoveryNotice.isEmpty()",
                driver_ui,
            )
            self.assertIn(
                "String requested = vulkanDrivers.get(position).selectionId",
                listener,
            )
            self.assertIn("if (requested.equals(currentDriver[0]))", listener)
            self.assertIn("selectVulkanDriver(requested, vulkanDriverSpinner)", listener)
            self.assertNotIn("sanitizeSelection(position)", listener)
            self.assertNotRegex(listener, r"putInt\([^;]*position")
            self.assertIn("registerImported(", SETTINGS)
            self.assertNotIn("activateImported(", SETTINGS)
            self.assertNotIn("VulkanDriverManager.selectDriver", braced_block(
                SETTINGS, "private void finishTurnipDriverImport("
            ))


if __name__ == "__main__":
    unittest.main()
