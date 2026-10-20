from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
USBD_CPP = ROOT / "funnel-arm/src/core/libraries/usbd/usbd.cpp"
LIBS_CPP = ROOT / "funnel-arm/src/core/libraries/libs.cpp"


def test_android_usbd_event_loop_is_a_real_registered_contract():
    usbd = USBD_CPP.read_text(encoding="utf-8")
    libs = LIBS_CPP.read_text(encoding="utf-8")

    assert 'LIB_FUNCTION("EkqGLxWC-S0"' in usbd
    assert "sceUsbdHandleEvents);" in usbd
    assert "event_cv.wait_until" in usbd
    assert "executor_live_mono_run_pending_signal_safe_point" in usbd
    assert "init_count.fetch_add(1" in usbd
    assert "return ORBIS_OK;" in usbd
    assert "Libraries::Usbd::RegisterLib(sym);" in libs


def test_android_usbd_reports_an_empty_device_set_without_title_rules():
    usbd = USBD_CPP.read_text(encoding="utf-8")

    assert "empty_list[0] = nullptr;" in usbd
    assert "CUSA" not in usbd
    assert "GetTitleId" not in usbd
