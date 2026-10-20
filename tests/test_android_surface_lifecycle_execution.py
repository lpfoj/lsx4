"""Execute production window publication and WSI transition code with a controlled driver.

Vulkan image allocation after SetSurfaceProperties is excluded from the host harness;
the Android build verifies the complete translation units. Native window ownership,
the Create surface transition prefix, and the full AcquireNextImage body are executed.
"""
from pathlib import Path
import os,sys,subprocess,tempfile,unittest
ROOT=Path(os.environ.get('LSX4_SURFACE_ROOT',Path(__file__).resolve().parents[1]))
sys.path.insert(0,str(ROOT))
from tests.native_lifecycle_compiler import compiler_command
SOURCE_ROOT=Path(os.environ.get('LSX4_SURFACE_SOURCE_ROOT',ROOT))
def extract(s,sig):
 a=s.index(sig);b=s.index('{',a);e=b+1;depth=1
 while depth:depth+=(s[e]=='{')-(s[e]=='}');e+=1
 return s[a:e]
PRE=r'''
#include <memory>
#include <mutex>
#include <cstdint>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
using u64=uint64_t;using u32=uint32_t;using s32=int32_t;
#define __ANDROID__ 1
#define VK_NULL_HANDLE nullptr
#define LOG_WARNING(...)
#define LOG_ERROR(...)
#define NativeLog(...)
// Signal behavior and the actual guarded WindowSDL getters are exercised by
// test_runtime_status_signal_mutex_execution; this harness isolates WSI ownership.
namespace Common { struct SignalPauseMask {}; }
void require(bool c,const char*m){if(!c)throw std::runtime_error(m);}
struct ANativeWindow{int refs=0;};
void ANativeWindow_acquire(ANativeWindow*p){++p->refs;}
void ANativeWindow_release(ANativeWindow*p){require(p->refs>0,"over-release");--p->refs;}
namespace Frontend {
struct WindowSystemInfo{void*render_surface=nullptr;std::shared_ptr<void>render_surface_owner;u64 surface_generation=0;};
struct WindowSDL{
 s32 width=0,height=0;mutable std::mutex android_surface_mutex;WindowSystemInfo window_info;
 WindowSDL(s32 w,s32 h,void*p){UpdateAndroidSurface(w,h,p);}
 void UpdateAndroidSurface(s32,s32,void*);
 // GETTERS
};
// UPDATE
}
bool g_surface_attached=false;void*g_native_window=nullptr;int g_surface_width=0,g_surface_height=0;
std::unique_ptr<Frontend::WindowSDL>g_android_vulkan_window;Frontend::WindowSDL*g_window=nullptr;
// REFRESH
namespace vk {
enum class Result {eSuccess,eErrorDeviceLost,eTimeout,eSuboptimalKHR,eErrorSurfaceLostKHR,eErrorOutOfDateKHR,eErrorUnknown};
std::string to_string(Result){return "mock";}
struct DeviceState{Result result=Result::eSuccess;int acquires=0;std::function<void()>during;};
struct Device{
 std::shared_ptr<DeviceState>s=std::make_shared<DeviceState>();
 Result acquireNextImageKHR(bool,u64,int,void*,u32*){++s->acquires;if(s->during)s->during();return s->result;}
};
}
struct Surface {ANativeWindow*n;};
std::vector<std::string>events;
struct MockInstance {
 vk::Device device;
 const MockInstance&GetInstance()const{return *this;}
 vk::Device GetDevice()const{return device;}
 void destroySurfaceKHR(Surface*p)const{if(p){require(p->n->refs>0,"native window released before surface");events.push_back("destroy_surface");delete p;}}
};
Surface*CreateSurface(const MockInstance&,const Frontend::WindowSystemInfo&i){
 require(i.render_surface_owner.get()==i.render_surface,"unleased surface snapshot");
 require(static_cast<ANativeWindow*>(i.render_surface)->refs>0,"unowned native window");
 events.push_back("create_surface");return new Surface{static_cast<ANativeWindow*>(i.render_surface)};
}
struct Swapchain {
 MockInstance instance;const Frontend::WindowSDL&window;
 Surface*surface=nullptr;std::shared_ptr<void>native_window_owner;u64 native_surface_generation=0;
 bool surface_lost=false,needs_recreation=true,terminal=false,swapchain=false;
 vk::Result terminal_result=vk::Result::eSuccess;u32 width=0,height=0,frame_index=0,image_index=0,consecutive_acquire_timeouts=0;
 std::vector<int>image_acquired;
 explicit Swapchain(const Frontend::WindowSDL&w):window(w){}
 ~Swapchain(){Destroy();instance.destroySurfaceKHR(surface);}
 void Destroy(){events.push_back("destroy_swapchain");swapchain=false;image_acquired.clear();}
 bool SetSurfaceProperties(){return true;}
 bool format_available=true;int format_queries=0;
 bool FindPresentFormat(){++format_queries;return format_available;}
 void FindPresentMode(){}
 void Create(u32,u32);
 bool AcquireNextImage();
};
// CREATE
// ACQUIRE
void attach(ANativeWindow*p,int w=960,int h=544){g_native_window=p;g_surface_attached=p!=nullptr;g_surface_width=p?w:0;g_surface_height=p?h:0;RefreshAndroidVulkanWindowLocked();}
'''
MAIN=r'''
int main(int argc,char**argv){try{
 const std::string c=argv[1];ANativeWindow a,b;attach(&a);auto*w=g_window;
 if(c=="stable"){
  auto old=w->GetWindowInfo();attach(&a);require(w==g_window,"same-surface callback replaced WindowSDL");
  require(old.surface_generation==w->GetWindowInfo().surface_generation,"duplicate callback invalidated WSI");
  attach(nullptr);require(w==g_window,"detach destroyed renderer's WindowSDL reference");
  require(a.refs==1,"snapshot did not retain old native window");
  attach(&b);require(w==g_window,"replacement invalidated renderer window reference");
  old={};require(a.refs==0&&b.refs==1,"window lease imbalance");
 }else{
  Swapchain s(*w);s.Create(960,544);require(s.AcquireNextImage(),"initial acquire");events.clear();
  auto driver=s.instance.device.s;
  if(c=="unchanged"){
   for(int i=0;i<100;++i){attach(&a);require(s.AcquireNextImage(),"duplicate attach interrupted frame");}
   require(events.empty(),"steady-state recreated surface");
   auto gen=w->GetWindowInfo().surface_generation;attach(&a,1280,720);
   require(w->GetWidth()==1280&&w->GetHeight()==720,"new dimensions not published");
   require(w->GetWindowInfo().surface_generation==gen,"resize replaced same native surface");
  }else if(c=="reattach"){
   attach(nullptr);int count=driver->acquires;
   for(int i=0;i<30;++i)require(!s.AcquireNextImage()&&!s.terminal,"background made renderer terminal");
   require(driver->acquires==count,"acquired abandoned BufferQueue");
   require(a.refs==1,"swapchain lost its native window lease");
   attach(&b);require(!s.AcquireNextImage()&&s.needs_recreation,"new native surface went unnoticed");
   s.Create(960,544);require(a.refs==0,"old native window leaked");
   require(events==std::vector<std::string>({"destroy_swapchain","destroy_surface","create_surface"}),"parent/child replacement order");
   require(s.AcquireNextImage(),"new surface did not resume");
  }else if(c=="lost"){
   driver->result=vk::Result::eErrorSurfaceLostKHR;require(!s.AcquireNextImage()&&s.surface_lost&&!s.terminal,"lost surface treated as device death");
   s.Create(960,544);require(events==std::vector<std::string>({"destroy_swapchain","destroy_surface","create_surface"}),"lost VkSurface was reused");
   driver->result=vk::Result::eSuccess;require(s.AcquireNextImage(),"lost surface did not recover");
  }else if(c=="timeout_detach"){
   s.consecutive_acquire_timeouts=7;driver->result=vk::Result::eTimeout;driver->during=[](){attach(nullptr);};
   require(!s.AcquireNextImage()&&!s.terminal,"detach racing last timeout killed renderer");
   driver->during={};attach(&b);s.Create(960,544);driver->result=vk::Result::eSuccess;
   require(s.AcquireNextImage(),"timeout/detach did not recover");
  }else if(c=="format_retry"){
   s.surface_lost=true;s.format_available=false;s.Create(960,544);
   require(!s.swapchain&&s.needs_recreation,"failed format created a swapchain");
   int queries=s.format_queries;s.format_available=true;s.Create(960,544);
   require(s.format_queries==queries+1&&s.AcquireNextImage(),"format failure was not queried again");
  }else if(c=="device_lost"){
   driver->result=vk::Result::eErrorDeviceLost;require(!s.AcquireNextImage()&&s.terminal,"device loss was swallowed");
   attach(&b);driver->result=vk::Result::eSuccess;require(!s.AcquireNextImage(),"surface replacement revived lost device");
  }
 }
 attach(nullptr);g_android_vulkan_window.reset();g_window=nullptr;
 require(a.refs==0&&b.refs==0,"native window reference leaked");
 std::cout<<"PASS "<<c<<'\n';return 0;
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 23;}}
'''
class Execution(unittest.TestCase):
 def test_production_surface_transitions(self):
  src=SOURCE_ROOT/'funnel-arm/src';h=(src/'sdl_window.h').read_text(encoding='utf-8');cpp=(src/'sdl_window.cpp').read_text(encoding='utf-8')
  getters='\n'.join(extract(h,s) for s in ['s32 GetWidth() const','s32 GetHeight() const','WindowSystemInfo GetWindowInfo() const'])
  update=extract(cpp,'void WindowSDL::UpdateAndroidSurface(')
  runtime=Path(os.environ.get('LSX4_SURFACE_OLD_REFRESH',SOURCE_ROOT/'src/executor/native_runtime_api.cpp')).read_text(encoding='utf-8');refresh=extract(runtime,'void RefreshAndroidVulkanWindowLocked()')
  swap=(src/'video_core/renderer_vulkan/vk_swapchain.cpp').read_text(encoding='utf-8')
  create=extract(swap,'void Swapchain::Create(').split('    const std::array queue_family_indices')[0]+'    swapchain=true;image_acquired={1};\n}'
  acquire=extract(swap,'bool Swapchain::AcquireNextImage()')
  code=PRE.replace('// GETTERS',getters).replace('// UPDATE',update).replace('// REFRESH',refresh).replace('// CREATE',create).replace('// ACQUIRE',acquire)+MAIN
  with tempfile.TemporaryDirectory(prefix='lsx4-surface-') as temp:
   p=Path(temp);source=p/'test.cpp';exe=p/('test.exe'if os.name=='nt'else'test');source.write_text(code,encoding='utf-8')
   cmd,env,msvc=compiler_command();args=cmd+[str(source),'/Fe:'+str(exe),'/Fo:'+str(p/'test.obj')]if msvc else cmd+[str(source),'-o',str(exe)]
   r=subprocess.run(args,cwd=p,env=env,capture_output=True,text=True);self.assertEqual(r.returncode,0,r.stdout+r.stderr)
   for case in (['stable']if os.environ.get('LSX4_SURFACE_OLD_REFRESH')else ['stable','unchanged','reattach','lost','timeout_detach','format_retry','device_lost']):
    with self.subTest(case=case):
     r=subprocess.run([str(exe),case],capture_output=True,text=True,timeout=10);self.assertEqual(r.returncode,0,r.stdout+r.stderr);print(r.stdout.strip())
if __name__=='__main__':unittest.main()
