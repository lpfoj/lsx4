// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#import <TargetConditionals.h>
#if TARGET_OS_IOS
#import <UIKit/UIKit.h>
// One small responder per game surface; starts/stops with its window lifetime.
void LSX4InstallSystemTextInput(UIView* host);
#endif
