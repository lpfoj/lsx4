// SPDX-License-Identifier: GPL-2.0-or-later
#import "AppleSystemTextInput.h"
#import "AppleTextInput.h"
#if TARGET_OS_IOS
#include <cstdint>

static NSUInteger SharedPrefix(NSString* a,NSString* b) {
    NSUInteger i=0,limit=MIN(a.length,b.length);
    while(i<limit && [a characterAtIndex:i]==[b characterAtIndex:i])++i;
    if(i && i<a.length && CFStringIsSurrogateHighCharacter([a characterAtIndex:i-1]))--i;
    return i;
}
static uint32_t CodePoints(NSString* value,NSUInteger start) {
    uint32_t count=0;
    for(NSUInteger i=start;i<value.length;++i,++count) {
        const unichar c=[value characterAtIndex:i];
        if(CFStringIsSurrogateHighCharacter(c) && i+1<value.length &&
           CFStringIsSurrogateLowCharacter([value characterAtIndex:i+1]))++i;
    }
    return count;
}
@interface LSX4SystemTextResponder : UITextView<UITextViewDelegate>
@end
@implementation LSX4SystemTextResponder {
    NSTimer* _poll;
    NSString* _committed;
    uint64_t _session;
    uint64_t _suppressed;
    BOOL _internal;
}
- (instancetype)initWithFrame:(CGRect)frame {
    if((self=[super initWithFrame:frame textContainer:nil])) {
        self.delegate=self;self.backgroundColor=UIColor.clearColor;self.textColor=UIColor.clearColor;
        self.tintColor=UIColor.clearColor;self.alpha=0.01;self.scrollEnabled=NO;
        self.autocapitalizationType=UITextAutocapitalizationTypeNone;
        self.autocorrectionType=UITextAutocorrectionTypeNo;
        self.spellCheckingType=UITextSpellCheckingTypeNo;
        self.smartQuotesType=UITextSmartQuotesTypeNo;self.smartDashesType=UITextSmartDashesTypeNo;
        self.returnKeyType=UIReturnKeyDone;
        self.accessibilityElementsHidden=YES;
        self.inputAssistantItem.leadingBarButtonGroups=@[];
        self.inputAssistantItem.trailingBarButtonGroups=@[];
        UIToolbar* toolbar=[[UIToolbar alloc] initWithFrame:CGRectMake(0,0,320,44)];
        toolbar.items=@[[[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemCancel
            target:self action:@selector(cancelText)],
            [[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemFlexibleSpace target:nil action:nil],
            [[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemDone target:self action:@selector(submitText)]];
        self.inputAccessoryView=toolbar;_committed=@"";
        [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(background:)
            name:UIApplicationWillResignActiveNotification object:nil];
    }
    return self;
}
- (void)didMoveToWindow {
    [super didMoveToWindow];[_poll invalidate];_poll=nil;
    if(!self.window){[self endKeyboard];return;}
    __weak LSX4SystemTextResponder* weakSelf=self;
    _poll=[NSTimer timerWithTimeInterval:0.075 repeats:YES block:^(NSTimer*){[weakSelf updateKeyboard];}];
    [NSRunLoop.mainRunLoop addTimer:_poll forMode:NSRunLoopCommonModes];
}
- (void)dealloc {[_poll invalidate];[NSNotificationCenter.defaultCenter removeObserver:self];}
- (void)background:(NSNotification*)notification {
    (void)notification;
    // Lifecycle dismissal never submits/cancels the guest dialog.
    [self endKeyboard];
}
- (void)endKeyboard {
    _session=0;_internal=YES;self.text=@"";_committed=@"";_internal=NO;
    [self resignFirstResponder];
}
- (void)updateKeyboard {
    if(!self.window || UIApplication.sharedApplication.applicationState!=UIApplicationStateActive) {
        if(_session)[self endKeyboard];return;
    }
    const uint64_t current=executor_lsx4_apple_text_session();
    if(!current){_suppressed=0;if(_session)[self endKeyboard];return;}
    if(current==_suppressed)return;
    if(current!=_session) {
        [self endKeyboard];_session=current;_suppressed=0;
        [self becomeFirstResponder];
    }
}
- (BOOL)appendCommitted:(NSString*)value {
    NSData* bytes=[value dataUsingEncoding:NSUTF8StringEncoding allowLossyConversion:NO];
    return bytes && bytes.length && bytes.length<=16384 &&
        executor_lsx4_apple_text_commit(_session,static_cast<const char*>(bytes.bytes),bytes.length)>0;
}
- (void)textViewDidChange:(UITextView*)textView {
    if(_internal || !_session || executor_lsx4_apple_text_session()!=_session)return;
    NSString* current=textView.text?:@"";
    if(textView.markedTextRange) {
        const NSInteger offset=[textView offsetFromPosition:textView.beginningOfDocument
            toPosition:textView.markedTextRange.start];
        if(offset>=0 && static_cast<NSUInteger>(offset)<=current.length) {
            NSString* prefix=[current substringToIndex:static_cast<NSUInteger>(offset)];
            if([prefix hasPrefix:_committed] && prefix.length>_committed.length &&
                [self appendCommitted:[prefix substringFromIndex:_committed.length]])_committed=prefix;
        }
        return; // Never forward provisional composition as committed text.
    }
    const NSUInteger common=SharedPrefix(_committed,current);
    if(common<_committed.length) {
        if(executor_lsx4_apple_text_backspace(_session,CodePoints(_committed,common))<=0)return;
        _committed=[_committed substringToIndex:common];
    }
    if(common<current.length && ![self appendCommitted:[current substringFromIndex:common]])return;
    _committed=current;
}
- (BOOL)textView:(UITextView*)textView shouldChangeTextInRange:(NSRange)range replacementText:(NSString*)text {
    if(!_session || executor_lsx4_apple_text_session()!=_session)return NO;
    if([text isEqualToString:@"\n"] && !textView.markedTextRange){[self submitText];return NO;}
    if(range.location>textView.text.length || range.length>textView.text.length-range.location)return NO;
    NSString* value=[textView.text stringByReplacingCharactersInRange:range withString:text];
    NSData* bytes=[value dataUsingEncoding:NSUTF8StringEncoding allowLossyConversion:NO];
    return value.length<=4096 && bytes && bytes.length<=16384;
}
- (void)deleteBackward {
    if(!self.text.length && !self.markedTextRange && _session &&
       executor_lsx4_apple_text_session()==_session) {
        // The hidden keyboard starts empty even when the guest field contains
        // an existing name. A deletion here must still reach that guest field.
        executor_lsx4_apple_text_backspace(_session,1);
        return;
    }
    [super deleteBackward]; // Nonempty edits are forwarded once by the delegate.
}
- (void)submitText {
    if(!_session || self.markedTextRange)return;
    [self textViewDidChange:self];
    // Queue pressure or a stale session can reject a pending edit. Do not
    // submit a partially forwarded name; leave the keyboard available to retry.
    if(![_committed isEqualToString:self.text?:@""])return;
    const uint64_t token=_session;
    if(executor_lsx4_apple_text_submit(token)>0){_suppressed=token;[self endKeyboard];}
}
- (void)cancelText {
    const uint64_t token=_session;
    if(token && executor_lsx4_apple_text_cancel(token)>0){_suppressed=token;[self endKeyboard];}
}
@end
void LSX4InstallSystemTextInput(UIView* host) {
    NSCAssert(NSThread.isMainThread,@"Text input attaches on the UIKit thread");
    for(UIView* child in host.subviews)if([child isKindOfClass:LSX4SystemTextResponder.class])return;
    [host addSubview:[[LSX4SystemTextResponder alloc] initWithFrame:CGRectMake(0,0,1,1)]];
}
#endif
