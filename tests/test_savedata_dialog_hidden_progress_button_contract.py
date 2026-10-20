from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
UI = ROOT / "funnel-arm/src/core/libraries/save_data/dialog/savedatadialog_ui.cpp"


def test_hidden_system_message_does_not_render_an_ok_button():
    source = UI.read_text(encoding="utf-8")
    function = source.split("void SaveDialogUi::DrawSystemMessage()", 1)[1].split(
        "void SaveDialogUi::DrawErrorCode()", 1
    )[0]

    assert "if (!sys_state.hide_ok)" in function
    assert function.index("if (!sys_state.hide_ok)") < function.index(
        'Button(sys_state.show_no ? "Yes" : "OK", BUTTON_SIZE)'
    )


def test_hidden_progress_can_only_render_the_explicit_cancel_action():
    source = UI.read_text(encoding="utf-8")
    function = source.split("void SaveDialogUi::DrawSystemMessage()", 1)[1].split(
        "void SaveDialogUi::DrawErrorCode()", 1
    )[0]

    assert "int count = sys_state.hide_ok ? 0 : 1;" in function
    assert "else if (sys_state.show_cancel)" in function
    assert "drew_button && (first_render" in function
