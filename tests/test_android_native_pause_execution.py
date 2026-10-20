"""Execute the native signal predicate and actual audio pause/output bodies."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest

from tests.native_lifecycle_compiler import compiler_command

ROOT = Path(__file__).resolve().parents[1]


def function(source, signature):
    a = source.index(signature)
    b = source.index('{', a)
    e, depth = b + 1, 1
    while depth:
        depth += (source[e] == '{') - (source[e] == '}')
        e += 1
    return source[a:e]


COMMON = r'''
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <span>
#include <stdexcept>
#include <thread>
#include <vector>
using namespace std::chrono_literals;
using u64=uint64_t;using u32=uint32_t;
void require(bool yes,const char*why){if(!yes)throw std::runtime_error(why);}
'''

SIGNAL = r'''
#include "common/signal_pause_gate.h"
int main(){try{
 Common::SignalPauseGate gate;
 std::mutex mutex;std::condition_variable cv;int waits=0,wakes=0;
 auto wake=[&](auto*){std::lock_guard lock(mutex);++wakes;cv.notify_all();};
 gate.SetPaused(true,wake);gate.SetPaused(false,wake);
 gate.WaitIfPaused([&](auto*,auto){++waits;});
 require(waits==0,"coalesced pause/resume stranded delayed signal");
 gate.SetPaused(true,wake);
 gate.WaitIfPaused([&](auto*state,auto expected){
  ++waits;gate.SetPaused(false,wake);
  require(state->load()!=expected,"resume before futex must reject expected value");
 });
 require(waits==1,"compare-and-park race did not complete");
 std::promise<void> entered;
 gate.SetPaused(true,wake);
 auto guest=std::async(std::launch::async,[&]{gate.WaitIfPaused([&](auto*state,auto expected){
  std::unique_lock lock(mutex);entered.set_value();
  cv.wait(lock,[&]{return state->load()!=expected;});
 });});
 entered.get_future().wait();
 require(guest.wait_for(10ms)==std::future_status::timeout,"guest escaped paused epoch");
 gate.SetPaused(true,wake);
 require(guest.wait_for(10ms)==std::future_status::timeout,"duplicate pause released guest");
 gate.SetPaused(false,wake);
 require(guest.wait_for(1s)==std::future_status::ready,"resume wake lost");guest.get();
 const int before=waits;gate.WaitIfPaused([&](auto*,auto){++waits;});
 require(waits==before,"late queued signal parked resumed guest");
 std::cout<<"PASS coalesced, compare-before-park, active wait, duplicate pause, stale signal\n";
}catch(const std::exception&e){std::cerr<<e.what();return 23;}}
'''

AUDIO = r'''
#define LOG_ERROR(...)
constexpr int ORBIS_AUDIO_OUT_NUM_PORTS=2;
struct Backend{
 std::atomic_bool paused{};std::atomic_int outputs{};int transitions=0;
 void SetPaused(bool p){paused=p;++transitions;}
 void Output(void*){require(!paused,"output reached paused stream");++outputs;}
 void OutputFloat(const float*,u32,u32){Output(nullptr);}
};
struct PortOut{std::mutex mutex,backend_mutex;std::unique_ptr<Backend>impl=std::make_unique<Backend>();
 u32 sample_rate=48000,buffer_frames=256;struct{u32 num_channels=2;}format_info;};
std::array<std::shared_ptr<PortOut>,2>port_table;
std::shared_mutex port_table_mutex;std::mutex port_allocation_mutex;
std::atomic_bool session_paused{},session_retiring{};
std::mutex session_pause_mutex;std::condition_variable session_pause_changed;
struct Scheduler{int notifications=0;void NotifyLifecycleChange(){++notifications;}}scheduler;
Scheduler&AudioScheduler(){return scheduler;}
bool IsHostOutputEnabled(){return true;}
struct BatchItem{std::shared_ptr<PortOut>port;bool mixed_to_main=false,has_normalized_output=false;
 void*Data(){return nullptr;}const float*NormalizedData(){return nullptr;}};
// FUNCTIONS
int main(){try{
 auto port=std::make_shared<PortOut>();port_table[0]=port;
 std::array<BatchItem,1>batch{BatchItem{port}};
 SetSessionPaused(true);require(port->impl->paused,"host stream not suspended");
 SetSessionPaused(true);require(port->impl->transitions==1,"duplicate pause touched backend");
 auto writer=std::async(std::launch::async,[&]{OutputBatch(batch);});
 require(writer.wait_for(25ms)==std::future_status::timeout,"in-flight batch was consumed during pause");
 require(port->impl->outputs==0,"paused queue lost samples");
 {std::unique_lock lock(port->backend_mutex,std::try_to_lock);require(lock.owns_lock(),"wait owns backend mutex");}
 SetSessionPaused(false);
 require(writer.wait_for(1s)==std::future_status::ready,"audio resume deadlocked");writer.get();
 require(port->impl->outputs==1&&!port->impl->paused,"retained batch did not output once");
 SetSessionPaused(true);auto closing=std::async(std::launch::async,[&]{OutputBatch(batch);});
 require(closing.wait_for(25ms)==std::future_status::timeout,"shutdown fixture not parked");
 {std::scoped_lock lock(session_pause_mutex);session_retiring=true;}session_pause_changed.notify_all();
 require(closing.wait_for(1s)==std::future_status::ready,"retirement left paused batch stuck");closing.get();
 require(port->impl->outputs==1,"retirement played stale audio");
 require(port_table[0]==port&&port->impl!=nullptr,"pause destroyed port identity");
 std::cout<<"PASS host stream pause, in-flight retention, unlocked wait, resume, retirement\n";
}catch(const std::exception&e){std::cerr<<e.what();return 23;}}
'''

PS5_AUDIO = r'''
using aaudio_result_t=int;constexpr int AAUDIO_OK=0,AAUDIO_ERROR_INVALID_STATE=-1;
constexpr int64_t BackendWriteTimeoutNs=20000000;
struct AAudioStream{std::atomic_bool paused{};std::atomic_int writes{};};
int AAudioStream_requestPause(AAudioStream*s){s->paused=true;return 0;}
int AAudioStream_requestStart(AAudioStream*s){s->paused=false;return 0;}
int AAudioStream_getXRunCount(AAudioStream*){return 0;}
const char*AAudio_convertResultToText(int){return "mock";}
int AAudioStream_write(AAudioStream*s,float*,int frames,int64_t){require(!s->paused,"PS5 wrote paused stream");++s->writes;return frames;}
uint64_t AudioMonotonicNanoseconds(){return 1;}
void RecordAudioBackendWrite(uint64_t,int,u32,u32){}
struct Ps5AudioBackend{AAudioStream*stream{};std::mutex stream_mutex;};
struct Port{std::shared_ptr<Ps5AudioBackend>backend;struct{u32 buffer_frames=256;}config;};
struct BatchItem{std::shared_ptr<Port>port;float*Output(){return nullptr;}};
struct Harness {
 std::mutex backend_mutex_,queue_mutex_;std::condition_variable_any queue_cv_;
 std::vector<std::weak_ptr<Ps5AudioBackend>>backends_;
 std::atomic_bool paused_{},stopping_{};
 // METHODS
};
int main(){try{
 AAudioStream stream;auto backend=std::make_shared<Ps5AudioBackend>();backend->stream=&stream;
 auto port=std::make_shared<Port>();port->backend=backend;
 std::array<BatchItem,1>batch{BatchItem{port}};Harness h;h.backends_.push_back(backend);
 h.SetPaused(true);require(stream.paused,"PS5 stream not paused");uint64_t completed{};
 auto writer=std::async(std::launch::async,[&]{return h.OutputBatch(batch,completed);});
 require(writer.wait_for(25ms)==std::future_status::timeout,"PS5 in-flight slot consumed while paused");
 {std::unique_lock lock(backend->stream_mutex,std::try_to_lock);require(lock.owns_lock(),"PS5 waiter owns stream mutex");}
 h.SetPaused(false);require(writer.wait_for(1s)==std::future_status::ready,"PS5 audio resume stuck");
 require(writer.get()==256&&stream.writes==1,"PS5 retained slot missing/duplicated");
 h.SetPaused(true);auto closing=std::async(std::launch::async,[&]{return h.OutputBatch(batch,completed);});
 require(closing.wait_for(25ms)==std::future_status::timeout,"PS5 stop fixture not parked");
 {std::scoped_lock lock(h.queue_mutex_);h.stopping_=true;}h.queue_cv_.notify_all();
 require(closing.wait_for(1s)==std::future_status::ready,"PS5 paused worker did not retire");
 require(closing.get()==0&&stream.writes==1,"PS5 stop played stale buffer");
 std::cout<<"PASS PS5 stream pause, retained slot, unlocked wait, resume, stop\n";
}catch(const std::exception&e){std::cerr<<e.what();return 23;}}
'''


class NativePauseExecution(unittest.TestCase):
    def execute(self, code):
        with tempfile.TemporaryDirectory(prefix='lsx4-native-pause-') as raw:
            folder = Path(raw)
            source = folder/'test.cpp'
            exe = folder/('test.exe' if os.name == 'nt' else 'test')
            source.write_text(COMMON+code, encoding='utf-8')
            command, env, msvc = compiler_command()
            include = str(ROOT/'funnel-arm/src')
            args = ([str(source), '/I'+include, '/Fe:'+str(exe), '/Fo:'+str(folder/'test.obj')]
                    if msvc else [str(source), '-I'+include, '-pthread', '-o', str(exe)])
            result = subprocess.run(command+args, cwd=folder, env=env, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
            print(result.stdout.strip())

    def test_signal_predicate_execution(self):
        self.execute(SIGNAL)

    def test_actual_audio_pause_and_output(self):
        source = (ROOT/'funnel-arm/src/core/libraries/audio/audioout.cpp').read_text(encoding='utf-8')
        bodies = '\n'.join(function(source, signature) for signature in (
            'static bool WaitForAudioSessionResume()', 'static void SetHostStreamsPaused(',
            'void SetSessionPaused(', 'static void OutputBatch('))
        self.execute(AUDIO.replace('// FUNCTIONS', bodies))

    def test_runtime_wakes_guest_before_touching_audio_locks(self):
        source = (ROOT/'src/executor/native_runtime_api.cpp').read_text(encoding='utf-8')
        body = function(source, 'extern "C" int executor_lsx4_runtime_set_paused(')
        self.assertLess(body.index('Libraries::AudioOut::SetSessionPaused(true)'),
                        body.index('DebugState.TryPauseGuestThreads()'))
        resume = body[body.index('// Wake guest lock owners'):]
        self.assertLess(resume.index('DebugState.ResumeGuestThreads()'),
                        resume.index('Libraries::AudioOut::SetSessionPaused(false)'))

    def test_actual_ps5_audio_pause_and_output(self):
        source = (ROOT/'src/executor/ps5_desktop/ps5_audio_scheduler.cpp').read_text(encoding='utf-8')
        bodies = '\n'.join(function(source, signature) for signature in (
            'void SetPaused(', '[[nodiscard]] std::int32_t OutputBatch('))
        self.execute(PS5_AUDIO.replace('// METHODS', bodies))


if __name__ == '__main__':
    unittest.main()
