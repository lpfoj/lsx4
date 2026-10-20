import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
TIME_H = (ROOT / "funnel-arm/src/core/libraries/kernel/time.h").read_text(
    encoding="utf-8"
)
TIME_CPP = (ROOT / "funnel-arm/src/core/libraries/kernel/time.cpp").read_text(
    encoding="utf-8"
)
RTC_CPP = (ROOT / "funnel-arm/src/core/libraries/rtc/rtc.cpp").read_text(
    encoding="utf-8"
)


class KernelTimeConversionRecordContractTests(unittest.TestCase):
    def test_ps4_time_conversion_record_layout_is_explicit(self):
        for contract in (
            "sizeof(OrbisTimesec) == 16",
            "offsetof(OrbisTimesec, t) == 0",
            "offsetof(OrbisTimesec, west_sec) == 8",
            "offsetof(OrbisTimesec, dst_sec) == 12",
        ):
            self.assertIn(contract, TIME_H)

    def test_local_to_utc_populates_the_full_record(self):
        declaration = TIME_H.split("sceKernelConvertLocaltimeToUtc", 1)[1]
        self.assertIn("OrbisTimesec* st", declaration)
        implementation = TIME_CPP.split("sceKernelConvertLocaltimeToUtc", 1)[1]
        implementation = implementation.split("namespace Dev", 1)[0]
        for write in ("st->t = utc_time", "st->west_sec =", "st->dst_sec ="):
            self.assertIn(write, implementation)
        self.assertNotIn("OrbisKernelTimezone* timezone", implementation)

    def test_android_conversion_is_date_aware_and_round_trip_symmetric(self):
        helper = TIME_CPP.split("struct AndroidLocalTimeInfo", 1)[1]
        helper = helper.split("static void EnsureClockInitialized", 1)[0]
        self.assertIn("localtime_r(&utc_time", helper)
        self.assertIn("local.tm_gmtoff", helper)
        self.assertIn("gmtime_r(&local_time", helper)
        self.assertIn("local.tm_isdst =", helper)
        self.assertIn("mktime(&local)", helper)

        local_to_utc = TIME_CPP.split("sceKernelConvertLocaltimeToUtc", 1)[1]
        local_to_utc = local_to_utc.split("namespace Dev", 1)[0]
        self.assertIn("ResolveAndroidLocalToUtc", local_to_utc)
        self.assertIn("param_2", local_to_utc)

        utc_to_local = TIME_CPP.split("sceKernelConvertUtcToLocaltime", 1)[1]
        utc_to_local = utc_to_local.split("posix_clock_settime", 1)[0]
        self.assertIn("ResolveAndroidUtcToLocal", utc_to_local)
        self.assertIn("time + info.utc_offset_seconds", utc_to_local)
        self.assertNotIn("sceKernelGettimezone", utc_to_local)

    def test_android_timezone_api_does_not_use_obsolete_gettimeofday_timezone(self):
        android_route = TIME_CPP.split("s32 PS4_SYSV_ABI posix_gettimeofday", 1)[1]
        android_route = android_route.split("s32 PS4_SYSV_ABI sceKernelGettimeofday", 1)[0]
        self.assertIn("ResolveAndroidUtcToLocal", android_route)
        self.assertIn("tz_minuteswest", android_route)
        self.assertIn("tz_dsttime", android_route)
        self.assertNotIn("gettimeofday(&tv, &tzz)", android_route)

    def test_rtc_uses_the_converted_seconds_without_reinterpreting_the_record(self):
        route = RTC_CPP.split("sceRtcConvertLocalTimeToUtc", 1)[1]
        route = route.split("sceRtcConvertUtcToLocalTime", 1)[0]
        self.assertIn("Kernel::OrbisTimesec conversion{}", route)
        self.assertIn("utc_seconds - local_seconds", route)
        self.assertIn("pTickUtc == nullptr", route)


if __name__ == "__main__":
    unittest.main()
