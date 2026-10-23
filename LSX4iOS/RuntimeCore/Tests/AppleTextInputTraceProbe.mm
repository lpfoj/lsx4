// SPDX-License-Identifier: GPL-2.0-or-later
// Actual trace parser/scheduler with immediate controlled dispatch, no device.
#import <Foundation/Foundation.h>
#include <cassert>
#include <fstream>
#include <string>
#include <vector>
#include <cstdint>
static std::vector<dispatch_block_t> scheduled;
static dispatch_time_t FakeTime(dispatch_time_t,std::int64_t delta){return delta;}
static void FakeAfter(dispatch_time_t,dispatch_queue_t,dispatch_block_t block){scheduled.push_back([block copy]);}
#define dispatch_time FakeTime
#define dispatch_after FakeAfter
#include "AppleTextInputTrace.h"
#undef dispatch_time
#undef dispatch_after
struct Call {std::uint64_t token;std::string action,text;unsigned count;};
static std::uint64_t active=7;
static std::vector<Call> calls;
extern "C" uint64_t executor_lsx4_apple_text_session(void){return active;}
extern "C" int executor_lsx4_apple_text_commit(uint64_t t,const char* p,size_t n){calls.push_back({t,"text",std::string(p,n),1});return 1;}
extern "C" int executor_lsx4_apple_text_backspace(uint64_t t,uint32_t n){calls.push_back({t,"backspace",{},n});return 1;}
extern "C" int executor_lsx4_apple_text_submit(uint64_t t){calls.push_back({t,"submit",{},1});return 1;}
extern "C" int executor_lsx4_apple_text_cancel(uint64_t t){calls.push_back({t,"cancel",{},1});return 1;}
static std::string path;
static bool Parse(std::string text,std::vector<AppleTextTraceEvent>& events){std::ofstream(path)<<text;return ReadAppleTextInputTrace(90,events);}
int main(int argc,char** argv){@autoreleasepool{
 assert(argc==2);path=argv[1];setenv("LSX4_PROBE_TEXT_INPUT_TRACE",path.c_str(),1);
 const std::string valid=R"([{"seconds":2,"session":"name","action":"text","text":"A\u0416"},{"seconds":3,"session":"name","action":"backspace","count":1},{"seconds":4,"session":"name","action":"text","text":"B"},{"seconds":5,"session":"name","action":"submit"}])";
 std::vector<AppleTextTraceEvent> events;assert(Parse(valid,events));assert(events.size()==4&&events[0].text=="A\xd0\x96");
 ScheduleAppleTextInputTrace(events);assert(scheduled.size()==4);
 for(auto block:scheduled)block();assert(calls.size()==4&&calls.back().action=="submit"&&calls[1].count==1);
 scheduled.clear();calls.clear();active=7;ScheduleAppleTextInputTrace(events);
 scheduled[0]();active=8;for(std::size_t i=1;i<scheduled.size();++i)scheduled[i]();
 assert(calls.size()==1); // A replacement dialog never inherits a trace group.
 scheduled.clear();calls.clear();active=0;ScheduleAppleTextInputTrace(events);
 scheduled[0]();active=7;for(std::size_t i=1;i<scheduled.size();++i)scheduled[i]();
 assert(calls.empty()); // No late binding of submit to a newly opened field.
 const std::vector<std::string> invalid={
  R"({})",R"([{}])",R"([{"seconds":true,"session":"a","action":"submit"}])",
  R"([{"seconds":90,"session":"a","action":"submit"}])",
  R"([{"seconds":-1,"session":"a","action":"submit"}])",
  R"([{"seconds":2,"session":"a","action":"text","text":"x"},{"seconds":1,"session":"a","action":"submit"}])",
  R"([{"seconds":1,"session":"a","action":"text","text":"x","extra":0}])",
  R"([{"seconds":1,"session":"a","action":"text","text":"\u0000"}])",
  R"([{"seconds":1,"session":"a","action":"text","text":""}])",
  R"([{"seconds":1,"session":"a","action":"backspace","count":true}])",
  R"([{"seconds":1,"session":"a","action":"backspace","count":1.5}])",
  R"([{"seconds":1,"session":"a","action":"backspace","count":0}])",
  R"([{"seconds":1,"session":"a","action":"backspace","count":16385}])",
  R"([{"seconds":1,"session":"a","action":"submit","text":"x"}])",
  R"([{"seconds":1,"session":"a","action":"submit"},{"seconds":2,"session":"a","action":"text","text":"x"}])",
  R"([{"seconds":1,"session":"bad label","action":"submit"}])",
  R"([{"seconds":1,"session":"a","action":"unknown"}])"
 };
 for(const auto& input:invalid){scheduled.clear();assert(!Parse(input,events));assert(scheduled.empty());}
 assert(Parse(R"([{"seconds":1,"session":"cancel","action":"cancel"}])",events));
 scheduled.clear();calls.clear();active=9;ScheduleAppleTextInputTrace(events);scheduled[0]();assert(calls.size()==1&&calls[0].action=="cancel");
 std::printf("APPLE_TEXT_TRACE_CONTRACT validUtf8=1 balancedActions=1 staleSession=1 noLateBinding=1 cancel=1 negativeFormats=%zu passed=1\n",invalid.size());
}}
