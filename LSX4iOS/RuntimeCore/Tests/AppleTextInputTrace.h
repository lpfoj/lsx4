// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#import <Foundation/Foundation.h>
#include "../AppleTextInput.h"
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <vector>

struct AppleTextTraceEvent {
    double seconds{};
    std::string session,action,text;
    unsigned count{1};
};
static bool ReadAppleTextInputTrace(unsigned duration,std::vector<AppleTextTraceEvent>& events) {
    const char* setting=std::getenv("LSX4_PROBE_TEXT_INPUT_TRACE");
    if(!setting)return true;
    NSData* data=[NSData dataWithContentsOfFile:@(setting)];
    if(!data || data.length>1024*1024)return false;
    NSError* error=nil;
    id root=[NSJSONSerialization JSONObjectWithData:data options:0 error:&error];
    if(error || ![root isKindOfClass:NSArray.class] || [root count]>128)return false;
    double previous=0;
    NSCharacterSet* invalid=[[NSCharacterSet characterSetWithCharactersInString:
        @"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-"] invertedSet];
    std::map<std::string,bool> terminal;
    std::vector<AppleTextTraceEvent> parsed;
    for(id value in root) {
        if(![value isKindOfClass:NSDictionary.class])return false;
        NSDictionary* item=value;
        for(id key in item)if(![@[@"seconds",@"session",@"action",@"text",@"count"] containsObject:key])return false;
        id time=item[@"seconds"];NSString* session=item[@"session"];NSString* action=item[@"action"];
        if(![time isKindOfClass:NSNumber.class] || CFGetTypeID((__bridge CFTypeRef)time)==CFBooleanGetTypeID() ||
           ![session isKindOfClass:NSString.class] || !session.length || session.length>64 ||
           [session rangeOfCharacterFromSet:invalid].location!=NSNotFound ||
           ![action isKindOfClass:NSString.class])return false;
        const double seconds=[time doubleValue];
        if(!std::isfinite(seconds) || seconds<previous || seconds>=duration)return false;
        previous=seconds;
        AppleTextTraceEvent event{seconds,session.UTF8String,action.UTF8String};
        if(terminal[event.session])return false;
        if([action isEqualToString:@"text"]) {
            if(item[@"count"] || ![item[@"text"] isKindOfClass:NSString.class])return false;
            NSData* bytes=[item[@"text"] dataUsingEncoding:NSUTF8StringEncoding allowLossyConversion:NO];
            if(!bytes || !bytes.length || bytes.length>16384)return false;
            event.text.assign(static_cast<const char*>(bytes.bytes),bytes.length);
            if(event.text.find('\0')!=std::string::npos)return false;
        } else if([action isEqualToString:@"backspace"]) {
            id count=item[@"count"];
            if(item[@"text"] || ![count isKindOfClass:NSNumber.class] ||
               CFGetTypeID((__bridge CFTypeRef)count)==CFBooleanGetTypeID() ||
               [count doubleValue]<1 || [count doubleValue]>16384 ||
               std::floor([count doubleValue])!=[count doubleValue])return false;
            event.count=[count unsignedIntValue];
        } else if([action isEqualToString:@"submit"] || [action isEqualToString:@"cancel"]) {
            if(item[@"text"] || item[@"count"])return false;
            terminal[event.session]=true;
        } else return false;
        parsed.push_back(std::move(event));
    }
    events=std::move(parsed);return true;
}
struct AppleTextTraceBinding { bool attempted{};std::uint64_t token{}; };
static void ScheduleAppleTextInputTrace(const std::vector<AppleTextTraceEvent>& events) {
    std::map<std::string,std::shared_ptr<AppleTextTraceBinding>> bindings;
    for(const auto& event:events) {
        auto& entry=bindings[event.session];if(!entry)entry=std::make_shared<AppleTextTraceBinding>();
        const auto binding=entry;
        // Objective-C blocks retain C++ reference variables as references.
        // The caller's event vector ends before delayed callbacks execute.
        const auto ownedEvent=std::make_shared<const AppleTextTraceEvent>(event);
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW,static_cast<int64_t>(event.seconds*NSEC_PER_SEC)),
            dispatch_get_main_queue(),^{
                const auto& event=*ownedEvent;
                if(!binding->attempted) {
                    binding->attempted=true;binding->token=executor_lsx4_apple_text_session();
                }
                const auto token=binding->token;int result=0;
                if(token && token==executor_lsx4_apple_text_session()) {
                    if(event.action=="text")result=executor_lsx4_apple_text_commit(token,event.text.data(),event.text.size());
                    else if(event.action=="backspace")result=executor_lsx4_apple_text_backspace(token,event.count);
                    else if(event.action=="submit")result=executor_lsx4_apple_text_submit(token);
                    else if(event.action=="cancel")result=executor_lsx4_apple_text_cancel(token);
                }
                std::fprintf(stderr,"[APPLE_TEXT_TRACE] seconds=%.3f session=%s action=%s bytes=%zu count=%u token=%llu result=%d\n",
                    event.seconds,event.session.c_str(),event.action.c_str(),event.text.size(),event.count,
                    static_cast<unsigned long long>(token),result);
            });
    }
}
