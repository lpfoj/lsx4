"""Run the production image-usage selection with controlled Vulkan format queries."""
import os
from pathlib import Path
import subprocess,tempfile,unittest,sys
ROOT=Path(os.environ.get('LSX4_IMAGE_TEST_ROOT',Path(__file__).resolve().parents[1]))
import shutil

def compiler_command():
    for name in ('c++', 'g++', 'clang++'):
        if compiler := shutil.which(name):
            return [compiler, '-std=c++20', '-O2'], os.environ.copy(), False
    vcvars = Path(os.environ.get('ProgramFiles', 'C:/Program Files')) / (
        'Microsoft Visual Studio/2022/Community/VC/Auxiliary/Build/vcvars64.bat'
    )
    if os.name == 'nt' and vcvars.is_file():
        result = subprocess.run(
            f'"{os.environ.get("COMSPEC", "cmd.exe")}" /d /s /c ""{vcvars}" >nul && set"',
            check=True, capture_output=True, text=True, encoding='mbcs'
        )
        env = os.environ.copy()
        for line in result.stdout.splitlines():
            if '=' in line and not line.startswith('='):
                key, value = line.split('=', 1)
                env[key.upper()] = value
        if compiler := shutil.which('cl.exe', path=env.get('PATH')):
            return [compiler, '/nologo', '/std:c++20', '/EHsc', '/O2'], env, True
    raise unittest.SkipTest('A host C++20 compiler is required')

PREAMBLE=r'''
#define VULKAN_HPP_NO_CONSTRUCTORS
#include <vulkan/vulkan.hpp>
#include <cstdint>
#include <iostream>
#include <vector>
#include <stdexcept>
using u32=std::uint32_t;
#define UNREACHABLE() throw std::runtime_error("unreachable")
enum class ImageBackingRole { Universal, Texture, Storage, RenderTarget, DepthTarget, VideoOut };
struct ImageInfo {
 struct { bool is_block=false,is_depth=false; } props;
 struct { u32 levels=1,layers=1; } resources;
 u32 num_samples=2;
 vk::Format pixel_format=vk::Format::eR16G16B16A16Sfloat;
 int type=0;
};
struct Profile {
 bool optimization_a750_role_image_usage=false;
 bool optimization_a750_narrow_image_flags=false;
 bool optimization_a750_bounded_image_format_list=false;
};
namespace Vulkan {
struct Instance {
 Profile profile;
 bool feedback=true;
 vk::SampleCountFlags universal{vk::SampleCountFlagBits::e1};
 vk::SampleCountFlags renderable{vk::SampleCountFlagBits::e1|vk::SampleCountFlagBits::e2|vk::SampleCountFlagBits::e4|vk::SampleCountFlagBits::e8};
 mutable std::vector<vk::ImageUsageFlags> queries;
 mutable bool saw_narrow=false,saw_format_list=false;
 const Profile& GetCapabilityProfile() const { return profile; }
 bool IsAttachmentFeedbackLoopLayoutSupported() const { return feedback; }
 vk::Format GetSupportedFormat(vk::Format f,vk::FormatFeatureFlags2) const { return f; }
 bool IsOptimalImageFormatSupported(vk::Format,vk::FormatFeatureFlags2) const { return true; }
 const Instance& GetPhysicalDevice() const { return *this; }
 struct Result { vk::Result result; vk::ImageFormatProperties2 value; };
 Result getImageFormatProperties2(const vk::PhysicalDeviceImageFormatInfo2& q) const {
  if(q.format==vk::Format::eUndefined||q.tiling!=vk::ImageTiling::eOptimal||q.type!=vk::ImageType::e2D)
   throw std::runtime_error("incomplete format query");
  queries.push_back(q.usage);
  saw_narrow |= !(q.flags&vk::ImageCreateFlagBits::eExtendedUsage);
  if(q.pNext) {
   const auto* list=static_cast<const vk::ImageFormatListCreateInfo*>(q.pNext);
   if(list->viewFormatCount!=1||list->pViewFormats[0]!=q.format) throw std::runtime_error("format list mismatch");
   saw_format_list=true;
  }
  Result r{vk::Result::eSuccess,{}};
  r.value.imageFormatProperties.sampleCounts=(q.usage&vk::ImageUsageFlagBits::eStorage)?universal:renderable;
  return r;
 }
};
}
static vk::ImageType ConvertImageType(int) { return vk::ImageType::e2D; }
static std::vector<vk::Format> BoundedImageViewFormats(const Vulkan::Instance* i,vk::Format,vk::Format f,vk::ImageCreateFlags,vk::FormatFeatureFlags2) {
 return i->profile.optimization_a750_bounded_image_format_list?std::vector<vk::Format>{f}:std::vector<vk::Format>{};
}
'''

SUFFIX=r'''
void require(bool v,const char* message) { if(!v) throw std::runtime_error(message); }
int main() {
 try {
  Vulkan::Instance driver; ImageInfo info;
  for(auto format:{vk::Format::eR8G8B8A8Unorm,vk::Format::eR16G16B16A16Sfloat}) {
   info.pixel_format=format;
   for(u32 samples:{2u,8u}) {
    info.num_samples=samples;
    auto texture=choose(&driver,info,ImageBackingRole::Texture);
    require(texture.scoped,"texture-first MSAA collapsed to universal storage usage");
    require(bool(texture.usage&vk::ImageUsageFlagBits::eColorAttachment),"sample conversion requires renderable backing");
    require(!(texture.usage&vk::ImageUsageFlagBits::eStorage),"unused storage must not cap MSAA");
    auto target=choose(&driver,info,ImageBackingRole::RenderTarget);
    require(target.usage==texture.usage,"MSAA backing depends on first binding role");
    auto storage=choose(&driver,info,ImageBackingRole::Storage);
    require(!storage.scoped&&bool(storage.usage&vk::ImageUsageFlagBits::eStorage),"real storage request lost usage");
   }
  }
  driver.universal=driver.renderable;
  require(!choose(&driver,info,ImageBackingRole::Texture).scoped,"universal support must retain established path");
  driver.universal=vk::SampleCountFlagBits::e1;driver.renderable=vk::SampleCountFlagBits::e1;
  require(!choose(&driver,info,ImageBackingRole::Texture).scoped,"no invented unsupported MSAA capability");
  driver.renderable=vk::SampleCountFlagBits::e1|vk::SampleCountFlagBits::e2|vk::SampleCountFlagBits::e4|vk::SampleCountFlagBits::e8;
  info.num_samples=1;
  require(!choose(&driver,info,ImageBackingRole::Texture).scoped,"single-sample anchor changed");
  info.num_samples=2;info.resources.layers=2;
  require(!choose(&driver,info,ImageBackingRole::Texture).scoped,"sample-copy array limitation bypassed");
  info.resources.layers=1;info.resources.levels=2;
  require(!choose(&driver,info,ImageBackingRole::Texture).scoped,"sample-copy mip limitation bypassed");
  info.resources.levels=1;info.props.is_depth=true;
  require(!choose(&driver,info,ImageBackingRole::Texture).scoped,"depth requested color usage");
  info.props.is_depth=false;info.props.is_block=true;
  require(!choose(&driver,info,ImageBackingRole::Texture).scoped,"compressed image requested color usage");
  info.props.is_block=false;
  driver.profile.optimization_a750_narrow_image_flags=true;
  driver.profile.optimization_a750_bounded_image_format_list=true;
  require(choose(&driver,info,ImageBackingRole::Texture).scoped&&driver.saw_narrow&&driver.saw_format_list,"exact final flags/list must be queried");
  driver.profile.optimization_a750_role_image_usage=true;
  auto existing=choose(&driver,info,ImageBackingRole::Texture);
  require(existing.scoped&&!(existing.usage&vk::ImageUsageFlagBits::eColorAttachment),"existing capability profile changed");
  std::cout<<"MSAA initial-role execution passed\n";
 } catch(const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
}
'''

def production_code():
 path=Path(os.environ.get('LSX4_IMAGE_SOURCE',ROOT/'funnel-arm/src/video_core/texture_cache/image.cpp'))
 s=path.read_text(encoding='utf-8')
 roles=s[s.index('static vk::ImageUsageFlags UniversalImageUsageFlags'):s.index('static vk::ImageType ConvertImageType')]
 features=s[s.index('static vk::FormatFeatureFlags2 FormatFeatureFlags'):s.index('UniqueImage::~UniqueImage')]
 ctor=s[s.index('Image::Image('):]
 selection=ctor[ctor.index('    role_scoped_usage ='):ctor.index('    if (info.props.is_depth)')]
 return PREAMBLE+roles+features+r'''
struct Policy { bool scoped;vk::ImageUsageFlags usage; };
Policy choose(const Vulkan::Instance* instance,const ImageInfo& info,ImageBackingRole initial_role) {
 bool role_scoped_usage=false;vk::ImageUsageFlags usage_flags;vk::FormatFeatureFlags2 format_features;
 vk::ImageCreateFlags flags=vk::ImageCreateFlagBits::eMutableFormat|vk::ImageCreateFlagBits::eExtendedUsage;
'''+selection+'return {role_scoped_usage,usage_flags};\n}\n'+SUFFIX

class MsaaInitialRoleExecution(unittest.TestCase):
 def test_texture_first_and_target_first_preserve_same_capabilities(self):
  command,env,msvc=compiler_command()
  with tempfile.TemporaryDirectory(prefix='lsx4-image-role-') as tmp:
   d=Path(tmp);cpp=d/'role.cpp';exe=d/('role.exe' if os.name=='nt' else 'role')
   cpp.write_text(production_code(),encoding='utf-8')
   include=ROOT/'externals/vulkan-headers/include'
   flags=[str(cpp),'/I'+str(include),'/Fe:'+str(exe)] if msvc else [str(cpp),'-I',str(include),'-o',str(exe)]
   p=subprocess.run(command+flags,cwd=tmp,env=env,capture_output=True,text=True,timeout=60)
   self.assertEqual(p.returncode,0,p.stdout+'\n'+p.stderr)
   p=subprocess.run([str(exe)],cwd=tmp,env=env,capture_output=True,text=True,timeout=10)
   self.assertEqual(p.returncode,0,p.stdout+'\n'+p.stderr)
   self.assertIn('MSAA initial-role execution passed',p.stdout)
if __name__=='__main__':unittest.main()
