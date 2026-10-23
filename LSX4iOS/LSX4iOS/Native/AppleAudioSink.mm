// SPDX-License-Identifier: GPL-2.0-or-later
#import <AVFoundation/AVFoundation.h>
#include "GuestAudio.h"
namespace Lsx4::Apple {
namespace {
class AppleAudioSink final:public GuestAudioSink {
    AVAudioEngine* __strong engine_;
    NSMutableDictionary<NSNumber*,AVAudioPlayerNode*>* __strong players_;
    std::mutex mutex_;
public:
    AppleAudioSink():engine_([AVAudioEngine new]),players_([NSMutableDictionary new]){}
    ~AppleAudioSink() override {[engine_ stop];}
    bool Initialize() override {
        std::scoped_lock lock(mutex_);
        @autoreleasepool {
            NSError* error=nil;
            return [AVAudioSession.sharedInstance setCategory:AVAudioSessionCategoryPlayback
                withOptions:AVAudioSessionCategoryOptionMixWithOthers error:&error];
        }
    }
    bool Open(std::uint32_t handle) override {
        std::scoped_lock lock(mutex_);
        @autoreleasepool {
            AVAudioPlayerNode* player=nil;
            @try {
                NSError* error=nil;
                if(![AVAudioSession.sharedInstance setActive:YES error:&error])return false;
                player=[AVAudioPlayerNode new];
                auto* format=[[AVAudioFormat alloc] initStandardFormatWithSampleRate:48000 channels:2];
                [engine_ attachNode:player];[engine_ connect:player to:engine_.mainMixerNode format:format];
                if(!engine_.running && ![engine_ startAndReturnError:&error]) {[engine_ detachNode:player];return false;}
                players_[@(handle)]=player;return true;
            }@catch(NSException*) {if(player && [engine_.attachedNodes containsObject:player])[engine_ detachNode:player];return false;}
        }
    }
    void Close(std::uint32_t handle) override {
        std::scoped_lock lock(mutex_);
        @autoreleasepool {
            AVAudioPlayerNode* player=players_[@(handle)];
            if(player){[player stop];[engine_ detachNode:player];[players_ removeObjectForKey:@(handle)];}
        }
    }
    bool Output(std::uint32_t handle,std::span<const float> samples) override {
        std::scoped_lock lock(mutex_);
        @autoreleasepool {
            AVAudioPlayerNode* player=players_[@(handle)];
            if(!player || samples.empty() || samples.size()%2)return false;
            @try {
                const auto frames=AVAudioFrameCount(samples.size()/2);
                auto* format=[[AVAudioFormat alloc] initStandardFormatWithSampleRate:48000 channels:2];
                auto* buffer=[[AVAudioPCMBuffer alloc] initWithPCMFormat:format frameCapacity:frames];
                if(!buffer || !buffer.floatChannelData)return false;
                buffer.frameLength=frames;
                for(unsigned i=0;i<frames;++i){buffer.floatChannelData[0][i]=samples[i*2];buffer.floatChannelData[1][i]=samples[i*2+1];}
                dispatch_semaphore_t completed=dispatch_semaphore_create(0);
                [player scheduleBuffer:buffer completionCallbackType:AVAudioPlayerNodeCompletionDataPlayedBack
                    completionHandler:^(AVAudioPlayerNodeCompletionCallbackType){dispatch_semaphore_signal(completed);}];
                if(!player.playing)[player play];
                if(dispatch_semaphore_wait(completed,dispatch_time(DISPATCH_TIME_NOW,2*NSEC_PER_SEC))!=0){[player stop];return false;}
                return engine_.running && player.playing;
            }@catch(NSException*) {return false;}
        }
    }
};
}
std::unique_ptr<GuestAudioSink> CreatePlatformAudioSink(){return std::make_unique<AppleAudioSink>();}
}
