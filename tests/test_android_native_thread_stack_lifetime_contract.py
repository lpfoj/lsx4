from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
THREAD_CPP = ROOT / "funnel-arm/src/core/thread.cpp"
THREAD_H = ROOT / "funnel-arm/src/core/thread.h"
PTHREAD_CPP = ROOT / "funnel-arm/src/core/libraries/kernel/threads/pthread.cpp"


def source(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def test_android_native_host_callbacks_do_not_share_recyclable_guest_stack():
    pthread_source = source(PTHREAD_CPP)
    native_route = pthread_source.split(
        "new_thread->native_thr = Core::NativeThread();", 2
    )[-1].split("ASSERT_MSG(ret == 0", 1)[0]

    assert "#ifdef __ANDROID__" in native_route
    assert "CreateDetachedDefaultStack(RunThread, new_thread)" in native_route
    assert "#else" in native_route
    assert "Create(RunThread, new_thread, &new_thread->attr)" in native_route


def test_android_native_runner_never_clears_guest_stack_from_host_sp():
    pthread_source = source(PTHREAD_CPP)
    runner = pthread_source.split("static void RunThread(void* arg)", 1)[1].split(
        "static void RunAndroidMappedGuestThread", 1
    )[0]
    clear = runner.split("ClearGuestThreadSignalMask", 1)[1].split(
        "void* ret =", 1
    )[0]

    assert "#ifndef __ANDROID__" in clear
    assert "ClearStack();" in clear


def test_detached_default_stack_is_owned_and_retired_by_bionic():
    header = source(THREAD_H)
    implementation = source(THREAD_CPP)
    method = implementation.split(
        "int NativeThread::CreateDetachedDefaultStack", 1
    )[1].split("void NativeThread::Exit", 1)[0]

    assert "#ifdef __ANDROID__" in header
    assert "CreateDetachedDefaultStack" in header
    assert "pthread_attr_setdetachstate(&pattr, PTHREAD_CREATE_DETACHED)" in method
    assert "pthread_create(pthr, &pattr" in method
    assert "pthread_attr_destroy(&pattr)" in method


def test_mapped_guest_backend_route_keeps_its_existing_lifecycle():
    pthread_source = source(PTHREAD_CPP)
    mapped_route = pthread_source.split(
        "if (executor_lsx4_android_should_suppress_guest_thread", 1
    )[1].split("return ret;", 1)[0]

    assert "CreateDefaultStack(RunAndroidMappedGuestThread, new_thread)" in mapped_route
    assert "CreateDetachedDefaultStack(RunAndroidMappedGuestThread" not in mapped_route
