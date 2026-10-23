// SPDX-License-Identifier: GPL-2.0-or-later
#import <Foundation/Foundation.h>

// Launch configuration is an optimization, never a title whitelist.
static NSDictionary* LSX4ResolveLaunchPlan(NSString* title, NSURL* bundle) {
    NSRegularExpression* valid=[NSRegularExpression regularExpressionWithPattern:@"^CUSA[0-9]{5}$" options:0 error:nil];
    if(![title isKindOfClass:NSString.class] || [valid numberOfMatchesInString:title options:0 range:NSMakeRange(0,title.length)]!=1) return nil;
    NSURL* profiles=[bundle URLByAppendingPathComponent:@"CpuProfiles" isDirectory:YES];
    NSData* launchData=[NSData dataWithContentsOfURL:[[profiles URLByAppendingPathComponent:@"LocalLaunch"] URLByAppendingPathComponent:[title stringByAppendingString:@".json"]]];
    id launch=launchData ? [NSJSONSerialization JSONObjectWithData:launchData options:0 error:nil] : nil;
    NSData* cpu=[NSData dataWithContentsOfURL:[profiles URLByAppendingPathComponent:[title stringByAppendingString:@".ini"]]];
    BOOL matched=[launch isKindOfClass:NSDictionary.class] && [launch[@"title"] isEqual:title] && [launch[@"environment"] isKindOfClass:NSDictionary.class] && cpu.length>0;
    if(!matched) {
        NSData* defaults=[NSData dataWithContentsOfURL:[bundle URLByAppendingPathComponent:@"GenericLaunch.json"]];
        launch=defaults ? [NSJSONSerialization JSONObjectWithData:defaults options:0 error:nil] : nil;
        cpu=[NSData dataWithContentsOfURL:[bundle URLByAppendingPathComponent:@"GenericCpuProfile.ini"]];
    }
    if(![launch isKindOfClass:NSDictionary.class] || ![launch[@"environment"] isKindOfClass:NSDictionary.class] || cpu.length==0) return nil;
    for(id key in launch[@"environment"]) {
        id value=launch[@"environment"][key];
        if(![key isKindOfClass:NSString.class] || [key length]==0 || [key containsString:@"="] || ![value isKindOfClass:NSString.class]) return nil;
    }
    return @{@"cpu":cpu,@"environment":launch[@"environment"],@"generic":@(!matched),@"title":title};
}
