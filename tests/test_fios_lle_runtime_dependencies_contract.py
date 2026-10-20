from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
STUBS = ROOT / "funnel-arm" / "src" / "core" / "aerolib" / "stubs.cpp"


def source() -> str:
    return STUBS.read_text(encoding="utf-8")


def test_fios_libc_runtime_dependencies_are_real_typed_overrides():
    text = source()

    expected = {
        "hcuQgD53UxM": "ExecutorLibcPrintf",
        "GMpvxPFW924": "ExecutorLibcVprintf",
        "wLlFkwG9UcQ": "ExecutorLibcTime",
        "hHlZQUnlxSM": "ExecutorKernelGetrusage",
    }
    for nid, function in expected.items():
        assert f'{{"{nid}"' in text
        assert f"&{function}" in text


def test_getrusage_writes_a_fully_initialized_guest_layout():
    text = source()

    assert "struct ExecutorGuestRusage" in text
    assert "static_assert(sizeof(ExecutorGuestRusage) == 144)" in text
    assert "ExecutorGuestRusage guest{}" in text
    assert "ExecutorWriteGuestBytes(usage_ptr, &guest, sizeof(guest))" in text


def test_printf_and_time_preserve_guest_abi_outputs():
    text = source()

    assert "ExecutorBuildDirectGuestVaList(va, {fmt_ptr, a1, a2, a3, a4, a5}, 8)" in text
    assert "ExecutorLibcVasprintf" in text
    assert "ExecutorWriteGuestQword(result_ptr, static_cast<u64>(guest_time))" in text
