"""Run the production g_lock wrapper with real contention and an injected signal API.

The signal seam models pending-signal delivery at pthread_sigmask(SIG_SETMASK)
on Windows. Device acceptance must still exercise the real Android signal path.
"""
from pathlib import Path
import os
import re
import subprocess
import tempfile
import unittest

from tests.native_lifecycle_compiler import compiler_command

ROOT = Path(__file__).resolve().parents[1]


def wrapper(source):
    start = source.index('class RuntimeStatusMutex final {')
    end = source.index('\n};', start) + len('\n};')
    return source[start:end]


HARNESS = r'''
#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <system_error>
#include <thread>
using namespace std::chrono_literals;
using sigset_t = std::uint64_t;
constexpr int SIGSLEEP = 1, SIG_BLOCK = 2, SIG_SETMASK = 3;
constexpr int SIGVTALRM = SIGSLEEP;
constexpr sigset_t sleep_bit = 2;
struct SignalState {
    std::atomic<sigset_t> mask{0};
    std::atomic<bool> pending{false}, in_handler{false}, resume{false};
    int error = 0;
};
thread_local SignalState default_state;
thread_local SignalState* signal_state = &default_state;
int sigemptyset(sigset_t* out) { *out = 0; return 0; }
int sigaddset(sigset_t* out, int number) { *out |= sigset_t{1} << number; return 0; }
int pthread_sigmask(int how, const sigset_t* set, sigset_t* old) {
    auto& s = *signal_state;
    if (s.error) return s.error;
    auto previous = s.mask.load();
    if (old) *old = previous;
    auto next = how == SIG_BLOCK ? previous | *set : *set;
    s.mask.store(next);
    if (!(next & sleep_bit) && s.pending.exchange(false)) {
        s.in_handler.store(true);
        while (!s.resume.load()) std::this_thread::yield();
        s.in_handler.store(false);
    }
    return 0;
}
void require(bool condition, const char* why) {
    if (!condition) throw std::runtime_error(why);
}
template<class P> void until(P predicate, const char* why) {
    auto deadline = std::chrono::steady_clock::now() + 2s;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() > deadline) throw std::runtime_error(why);
        std::this_thread::yield();
    }
}
__WRAPPER__
RuntimeStatusMutex status_mutex;
int main() { try {
    signal_state->mask = 0x40;
    status_mutex.lock();
    require(signal_state->mask == (0x40 | sleep_bit), "pause signal not masked while locked");
    status_mutex.unlock();
    require(signal_state->mask == 0x40, "original signal mask not restored");
    signal_state->mask = 0x40 | sleep_bit;
    require(status_mutex.try_lock(), "try_lock uncontended");
    status_mutex.unlock();
    require(signal_state->mask == (0x40 | sleep_bit), "previously blocked pause was unblocked");
    signal_state->mask = 0x40;

    status_mutex.lock();
    std::thread contender([] {
        signal_state->mask = 0x80;
        require(!status_mutex.try_lock(), "try_lock acquired an owned mutex");
        require(signal_state->mask == 0x80, "failed try_lock leaked blocked mask");
    });
    contender.join();
    status_mutex.unlock();
    require(signal_state->mask == 0x40, "another thread overwrote owner's saved mask");

    // A queued pause cannot park the owner until after the shared mutex is free.
    SignalState owner;
    owner.mask = 0x80;
    std::atomic<bool> acquired{false}, release_owner{false};
    std::thread guest([&] {
        signal_state = &owner;
        status_mutex.lock();
        acquired = true;
        until([&] { return release_owner.load(); }, "owner not released");
        require(!owner.in_handler, "signal interrupted protected owner");
        status_mutex.unlock();
        require(owner.mask == 0x80, "guest prior signal mask lost");
    });
    until([&] { return acquired.load(); }, "owner did not acquire");
    owner.pending = true;
    release_owner = true;
    until([&] { return owner.in_handler.load(); }, "pending pause was not delivered on unlock");
    bool acquired_while_paused = status_mutex.try_lock();
    if (acquired_while_paused) status_mutex.unlock();
    owner.resume = true;
    guest.join();
    require(acquired_while_paused, "pause handler retained g_lock; Activity cannot resume");
    require(signal_state->mask == 0x40, "UI prior mask changed");

    // Mask before waiting for a contested mutex, not only after acquiring it.
    SignalState waiting;
    waiting.mask = 0x100;
    std::atomic<bool> entered{false};
    status_mutex.lock();
    std::thread waiter([&] {
        signal_state = &waiting;
        status_mutex.lock();
        entered = true;
        status_mutex.unlock();
    });
    until([&] { return (waiting.mask.load() & sleep_bit) != 0; }, "contended lock did not mask");
    waiting.pending = true;
    require(!entered, "mutex mutual exclusion broken");
    status_mutex.unlock();
    until([&] { return waiting.in_handler.load(); }, "contended pending pause not delivered");
    bool contended_released = status_mutex.try_lock();
    if (contended_released) status_mutex.unlock();
    waiting.resume = true;
    waiter.join();
    require(entered && contended_released, "contended owner parked before releasing mutex");
    require(waiting.mask == 0x100, "contended owner's prior mask not restored");

    signal_state->error = 22;
    bool threw = false;
    try { status_mutex.lock(); } catch (const std::system_error&) { threw = true; }
    signal_state->error = 0;
    require(threw && signal_state->mask == 0x40, "failed signal mask changed state");
    require(status_mutex.try_lock(), "failed mask acquired mutex");
    status_mutex.unlock();
    std::cout << "signal-safe g_lock: ownership, prior mask, try_lock failure, delayed pause, contention PASS\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n'; return 1;
} }
'''


class RuntimeStatusSignalMutexExecution(unittest.TestCase):
    def production_wrapper(self):
        path = Path(os.environ.get('LSX4_RUNTIME_MUTEX_SOURCE',
                                   ROOT / 'src/executor/native_runtime_api.cpp'))
        return wrapper(path.read_text(encoding='utf-8'))

    def run_harness(self, code):


        for name in ('sigset_t', 'sigemptyset', 'sigaddset', 'pthread_sigmask',
                     'SIGSLEEP', 'SIGVTALRM', 'SIG_BLOCK', 'SIG_SETMASK'):
            code = re.sub(r'\b' + name + r'\b', 'test_' + name, code)
        command, env, msvc = compiler_command()
        with tempfile.TemporaryDirectory(prefix='lsx4-signal-mutex-') as directory:
            root = Path(directory)
            source = root / 'run.cpp'
            binary = root / ('run.exe' if os.name == 'nt' else 'run')
            source.write_text(code, encoding='utf-8')
            args = command + [str(source)]
            args += ['/Fe:' + str(binary)] if msvc else ['-pthread', '-o', str(binary)]
            built = subprocess.run(args, cwd=root, env=env, text=True,
                                   capture_output=True, timeout=60)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            ran = subprocess.run([str(binary)], text=True, capture_output=True, timeout=10)
            self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)

    def test_production_wrapper_contention_and_pending_signal(self):
        self.run_harness(HARNESS.replace('__WRAPPER__', self.production_wrapper()))

    def test_underlying_lock_exceptions_restore_mask(self):


        code = HARNESS.split('__WRAPPER__')[0] + r'''
struct ThrowingMutex {
    void lock() { throw std::system_error(22, std::generic_category()); }
    bool try_lock() { throw std::system_error(22, std::generic_category()); }
    void unlock() { std::terminate(); }
};
'''
        actual = self.production_wrapper()
        self.assertIn('std::mutex mutex;', actual)
        code += actual.replace('std::mutex mutex;', 'ThrowingMutex mutex;') + r'''
int main() { try {
    RuntimeStatusMutex status_mutex;
    signal_state->mask = 0x40;
    bool threw = false;
    try { status_mutex.lock(); } catch (const std::system_error&) { threw = true; }
    require(threw && signal_state->mask == 0x40, "lock exception leaked signal mask");
    threw = false;
    try { status_mutex.try_lock(); } catch (const std::system_error&) { threw = true; }
    require(threw && signal_state->mask == 0x40, "try_lock exception leaked signal mask");
    return 0;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n'; return 1;
} }
'''
        self.run_harness(code)

    def test_actual_window_accessors_pause_after_releasing_lock(self):
        from tests.test_android_native_pause_execution import function
        header = (ROOT / 'funnel-arm/src/common/signal_pause_mask.h').read_text(encoding='utf-8')
        begin = header.index('class SignalPauseMask final {')
        guard = header[begin:header.index('\n};', begin) + len('\n};')]
        window_header = (ROOT / 'funnel-arm/src/sdl_window.h').read_text(encoding='utf-8')
        getters = '\n'.join(function(window_header, name) for name in
                            ('s32 GetWidth() const', 's32 GetHeight() const',
                             'WindowSystemInfo GetWindowInfo() const'))
        update = function((ROOT / 'funnel-arm/src/sdl_window.cpp').read_text(encoding='utf-8'),
                          'void WindowSDL::UpdateAndroidSurface(').replace('WindowSDL::', '')
        code = HARNESS.split('__WRAPPER__')[0] + '\nnamespace Common {\n' + guard + '\n}\n'
        code += self.production_wrapper() + r'''
#define __ANDROID__ 1
using s32 = std::int32_t;
struct ANativeWindow { int refs = 0; };
void ANativeWindow_acquire(ANativeWindow* p) { ++p->refs; }
void ANativeWindow_release(ANativeWindow* p) { --p->refs; }
struct WindowSystemInfo {
    void* render_surface = nullptr;
    std::shared_ptr<void> render_surface_owner;
    std::uint64_t surface_generation = 0;
};
struct InterceptMutex {
    std::mutex mutex;
    std::atomic<bool> entered{false}, proceed{false};
    void lock() {
        require((signal_state->mask.load() & sleep_bit) != 0, "snapshot lock taken before signal mask");
        mutex.lock(); entered = true;
        until([&] { return proceed.load(); }, "snapshot owner not released");
    }
    bool try_lock() { return mutex.try_lock(); }
    void unlock() { mutex.unlock(); }
};
struct Window {
    s32 width = 1600, height = 900;
    WindowSystemInfo window_info;
    mutable InterceptMutex android_surface_mutex;
__GETTERS__
__UPDATE__
};
int main() { try {
    RuntimeStatusMutex status_mutex;
    signal_state->mask = 0x40;
    status_mutex.lock();
    { Common::SignalPauseMask nested; }
    require(signal_state->mask == (0x40 | sleep_bit), "nested scope unmasked outer g_lock");
    status_mutex.unlock();
    require(signal_state->mask == 0x40, "nested scope lost original mask");
    ANativeWindow native;
    Window window;
    for (int operation = 0; operation < 4; ++operation) {
        SignalState owner;
        owner.mask = 0x80;
        auto& mutex = window.android_surface_mutex;
        mutex.entered = false; mutex.proceed = false;
        std::thread guest([&] {
            signal_state = &owner;
            if (operation == 0) require(window.GetWidth() == 1600, "width changed");
            if (operation == 1) require(window.GetHeight() == 900, "height changed");
            if (operation == 2) require(window.GetWindowInfo().render_surface == nullptr, "snapshot changed");
            if (operation == 3) window.UpdateAndroidSurface(1920, 1080, &native);
        });
        until([&] { return mutex.entered.load(); }, "window operation did not acquire");
        owner.pending = true;
        mutex.proceed = true;
        until([&] { return owner.in_handler.load(); }, "window pause not delivered");
        bool available = mutex.try_lock();
        if (available) mutex.unlock();
        owner.resume = true;
        guest.join();
        require(available, "paused snapshot owner blocked Activity surface update");
        require(owner.mask == 0x80, "window guard lost prior mask");
    }
    require(window.width == 1920 && window.height == 1080 && native.refs == 1,
            "surface update did not retain native window");
    signal_state->error = 22;
    bool threw = false;
    try { Common::SignalPauseMask guard; } catch (const std::system_error&) { threw = true; }
    signal_state->error = 0;
    require(threw && signal_state->mask == 0x40, "failed scope mask modified original mask");
    return 0;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n'; return 1;
} }
'''
        self.run_harness(code.replace('__GETTERS__', getters).replace('__UPDATE__', update))


if __name__ == '__main__':
    unittest.main()
