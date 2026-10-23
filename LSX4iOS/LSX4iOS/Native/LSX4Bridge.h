// SPDX-License-Identifier: GPL-2.0-or-later
#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>

NS_ASSUME_NONNULL_BEGIN
// A single-use execution lifetime. Cancellation stays set even before runFolder
// starts, so a quick back tap cannot leave an invisible game running.
@interface LSX4GameSession : NSObject
- (NSDictionary<NSString *, id> *)runFolder:(NSString *)folder;
- (void)cancel;
@end
@interface LSX4Bridge : NSObject
+ (BOOL)isDebuggerAttached;
+ (void)updateApplicationActive:(BOOL)active;
+ (void)updatePad:(uint32_t)buttons axes:(NSData*)axes connected:(BOOL)connected timestamp:(uint64_t)timestamp
     touchActive:(BOOL)touchActive touchX:(uint16_t)touchX touchY:(uint16_t)touchY;
+ (NSDictionary<NSString *, id> *)diagnostics;
+ (NSDictionary<NSString *, id> *)runJITProbe;
+ (NSDictionary<NSString *, id> *)inspectExecutable:(NSString *)path;
+ (NSDictionary<NSString *, id> *)runGuestTitle:(NSString *)folder;
+ (void)diagnosticsReportReady;
@end
@interface LSX4Bridge (VideoOutput)
+ (UIView*)makeVideoView;
+ (NSUInteger)presentedFrameCount;
+ (NSUInteger)droppedFrameCount;
+ (double)displayedFramesPerSecond;
+ (NSString*)presentationDetail;
#if DEBUG
+ (NSDictionary*)runVideoProbe;
#endif
@end
NS_ASSUME_NONNULL_END
