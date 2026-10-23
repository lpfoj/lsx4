#pragma once
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include "AndroidDisplayScaling.h"
namespace AndroidRenderLaunch {
struct Profile { const char* title; int outputWidth,outputHeight,displayMode; const char* ini; };
inline constexpr Profile profiles[]={{"global",1280,720,0,"version=4\nfsr_enabled=1\nupscaler_mode=1\nsource_mode=4\nsource_width=960\nsource_height=544\noutput_mode=2\noutput_width=1280\noutput_height=720\nsharp_bilinear_strength_milli=1000\nbicubic_kernel=0\nfsr1_rcas_enabled=1\nfsr1_rcas_sharpness_milli=750\nsgsr1_edge_sharpness_milli=2000\nnis_sharpness_milli=500\nsgsr2_temporal_stability_milli=700\narm_asr_sharpening=1\narm_asr_sharpness_milli=250\n"},{"PPSA00000",1280,720,0,"version=4\nfsr_enabled=1\nupscaler_mode=1\nsource_mode=4\nsource_width=960\nsource_height=544\noutput_mode=2\noutput_width=1280\noutput_height=720\nsharp_bilinear_strength_milli=1000\nbicubic_kernel=0\nfsr1_rcas_enabled=1\nfsr1_rcas_sharpness_milli=750\nsgsr1_edge_sharpness_milli=2000\nnis_sharpness_milli=500\nsgsr2_temporal_stability_milli=700\narm_asr_sharpening=1\narm_asr_sharpness_milli=250\n"}};
inline bool Enabled(){const char* p=std::getenv("LSX4_APPLE_RENDER_LAUNCH");return p&&std::strcmp(p,"on")==0;}
inline const Profile& Find(const char* title){
 for(const auto& p:profiles)if(std::strcmp(p.title,title)==0)return p;
 if(std::strncmp(title,"PPSA",4)==0)return Find("PPSA00000");
 return profiles[0];
}
inline const Profile& Current(){
 const char* game=std::getenv("LSX4_CORE_GAME");
 const auto title=game?std::filesystem::path(game).parent_path().filename().string():std::string{};
 return Find(title.c_str());
}
inline bool WriteBeforeCpu(const std::filesystem::path& root,const char* title){
 if(!Enabled())return true;
 const auto& p=Find(title);std::error_code error;
 const auto directory=root/"runtime-config";std::filesystem::create_directories(directory,error);
 if(error)return false;
 const auto target=directory/"android-render-resolution.ini",pending=directory/"android-render-resolution.ini.pending";
 {std::ofstream output(pending,std::ios::trunc|std::ios::binary);output<<p.ini;output.close();if(!output)return false;}
 std::filesystem::rename(pending,target,error);if(error)return false;
 std::fprintf(stderr,"[APPLE_ANDROID_RENDER_LAUNCH] profile=%s output=%dx%d displayMode=%d\n",p.title,p.outputWidth,p.outputHeight,p.displayMode);
 return true;
}
}
