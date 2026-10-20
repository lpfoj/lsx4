"""Execute sparse publication and the production tiler mip/extent predicate."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from tests.test_ps4_shader_mbcnt_execution import compiler_command
from tests.test_pipeline_warmup_runtime_info_contract import cpp_function

ROOT = Path(__file__).resolve().parents[1]


def execute(source):
    compiler, env, msvc = compiler_command()
    with tempfile.TemporaryDirectory(prefix='lsx4-readback-') as temp:
        folder = Path(temp)
        cpp = folder/'check.cpp'
        exe = folder/('check.exe' if msvc else 'check')
        cpp.write_text(source, encoding='utf-8')
        args = [*compiler, str(cpp), *([f'/Fe:{exe}'] if msvc else ['-o', str(exe)])]
        compiled = subprocess.run(args, cwd=folder, env=env, capture_output=True, text=True)
        if compiled.returncode:
            raise AssertionError(compiled.stdout+compiled.stderr)
        result = subprocess.run([str(exe)], capture_output=True, text=True)
        if result.returncode:
            raise AssertionError(result.stdout+result.stderr)
        return result.stdout


class ImageReadbackPreservationTests(unittest.TestCase):
    def test_sparse_gpu_changes_preserve_later_cpu_writes_and_guards(self):
        header=(ROOT/'funnel-arm/src/common/changed_bytes.h').as_posix()
        result=execute(r'''
#include <vector>
#include <cstdio>
#include <stdexcept>
#include "__HEADER__"
int main() {
  unsigned cases=0;
  for (std::size_t size : {0,1,2,7,8,31,63,64,65,127,128,129,4095,4096,4097,16896}) {
    for (unsigned seed=0;seed<40;++seed) {
      std::vector<std::uint8_t> old(size+32), gpu(size+32), cpu(size+32), expected;
      for (std::size_t i=0;i<old.size();++i) {
        old[i]=(i*17+seed*13)&255;gpu[i]=old[i];cpu[i]=old[i]^0x5a;
      }
      // The CPU updates arbitrary bytes after the GPU preservation snapshot.
      // Include tails, unaligned runs, all-changed and all-unchanged copies.
      for (std::size_t i=16;i<size+16;++i)
        if (seed==1 || (seed>1 && ((i*71+seed)%43)<13)) gpu[i]^=0xa5;
      expected=cpu;
      for (std::size_t i=16;i<size+16;++i)
        if (gpu[i]!=old[i]) expected[i]=gpu[i];
      Common::CopyChangedBytes(cpu.data()+16,gpu.data()+16,old.data()+16,size);
      if(cpu!=expected) throw std::runtime_error("padding or CPU write overwritten");
      ++cases;
    }
  }
  std::printf("preserved publication cases=%u PASS\n",cases);
}
'''.replace('__HEADER__',header))
        self.assertIn('cases=640 PASS',result)
        print(result.strip())

    def test_tiler_rejects_uncopied_mips_rows_slices_and_dispatch_tail(self):
        shader=(ROOT/'funnel-arm/src/video_core/host_shaders/tiling.comp').read_text()
        methods=cpp_function(shader,'uint GetMipLevel(')+'\n'+cpp_function(shader,'bool GetTiledIndex(')
        methods=methods.replace('inout uint texel','uint& texel').replace('out uint tiled_index','uint& tiled_index')


        source=r'''
#include <array>
#include <cstdio>
#include <stdexcept>
using uint=unsigned;
#define BYTES_PER_PIXEL 4u
#define IS_TILER 1
#define ARRAY_MODE 2
#define ARRAY_1D_TILED_THIN1 2
#define ARRAY_1D_TILED_THICK 3
struct uvec4 { uint x,y,z,w; };
struct Mip {uint size,pitch,height,offset;};
struct Info {uint num_mips; std::array<Mip,16> mips; std::array<uvec4,16> copied_extents;} info;
uint ComputeSurfaceAddrFromCoordMicroTiled(uint x,uint y,uint slice,uint pitch,uint height,uint) {
 return ((slice*height+y)*pitch+x)*BYTES_PER_PIXEL;
}
__METHODS__
int main() {
 unsigned cases=0;
 for(uint missing=0;missing<5;++missing) {
   info={};info.num_mips=4;
   uint total=0;
   for(uint mip=0;mip<4;++mip) {
     uint pitch=32u>>mip,height=8,slices=3;
     info.mips[mip]={pitch*height*slices*4,pitch,height,total*4};
     info.copied_extents[mip]=mip==missing?uvec4{}:uvec4{pitch-1,3,2,0};
     total+=pitch*height*slices;
   }
   for(uint linear=0;linear<total+64;++linear) {
     uint index=~0u;bool actual=GetTiledIndex(linear,index);
     bool expected=false;
     for(uint mip=0;mip<4;++mip) {
       auto m=info.mips[mip];uint first=m.offset/4,count=m.size/4;
       if(linear<first || linear>=first+count || mip==missing) continue;
       uint local=linear-first;
       expected=(local%m.pitch)<m.pitch-1 && (local/m.pitch)%m.height<3
           && local/(m.pitch*m.height)<2;
     }
     if(actual!=expected || (actual && index!=linear))
       throw std::runtime_error("tiler published an undefined texel");
     ++cases;
   }
 }
 info.num_mips=0;uint index=0;
 if(GetTiledIndex(0,index))throw std::runtime_error("empty copy accepted");
 std::printf("tiler coverage cases=%u PASS\n",cases);
}
'''.replace('__METHODS__',methods)
        result=execute(source)
        self.assertIn('cases=7520 PASS',result)
        print(result.strip())


if __name__=='__main__':
    unittest.main()
