// SPDX-License-Identifier: GPL-2.0-or-later
// Physical-device fixture controls. The production library UI has a separate
// session lifecycle and must not pretend that this probe supports cancellation.
#import <UIKit/UIKit.h>
#include <cstdint>
#include "../AppleConsoleInput.h"

@interface LSX4ProbeControls : UIView
@property(strong,nonatomic) NSMutableArray<UIButton*>* buttons;
@property(strong,nonatomic) UILabel* counter;
- (void)setInputReady:(BOOL)ready;
- (void)releaseAllButtons;
@end

@implementation LSX4ProbeControls
- (instancetype)initWithFrame:(CGRect)frame {
    if ((self=[super initWithFrame:frame])) {
        self.multipleTouchEnabled=YES;
        self.buttons=[NSMutableArray new];
        NSArray<NSString*>* titles=@[@"↑",@"←",@"↓",@"→",@"△",@"□",@"×",@"○",@"L1",@"R1",@"OPTIONS"];
        const uint32_t masks[]={0x10,0x80,0x40,0x20,0x1000,0x8000,0x4000,0x2000,0x400,0x800,0x8};
        for (NSUInteger i=0;i<titles.count;++i) {
            UIButton* button=[UIButton buttonWithType:UIButtonTypeCustom];
            button.tag=masks[i];
            button.accessibilityLabel=titles[i];
            [button setTitle:titles[i] forState:UIControlStateNormal];
            button.titleLabel.font=[UIFont systemFontOfSize:i==10?12:24 weight:UIFontWeightSemibold];
            button.backgroundColor=[UIColor.blackColor colorWithAlphaComponent:0.42];
            button.layer.cornerRadius=12;
            button.layer.borderWidth=1;
            button.layer.borderColor=[UIColor.whiteColor colorWithAlphaComponent:0.45].CGColor;
            button.enabled=NO;
            [button addTarget:self action:@selector(buttonDown:) forControlEvents:UIControlEventTouchDown|UIControlEventTouchDragEnter];
            [button addTarget:self action:@selector(buttonUp:) forControlEvents:UIControlEventTouchUpInside|UIControlEventTouchUpOutside|UIControlEventTouchCancel|UIControlEventTouchDragExit];
            [self.buttons addObject:button];
            [self addSubview:button];
        }
        self.counter=[UILabel new];
        self.counter.font=[UIFont monospacedDigitSystemFontOfSize:13 weight:UIFontWeightMedium];
        self.counter.textColor=UIColor.whiteColor;
        self.counter.backgroundColor=[UIColor.blackColor colorWithAlphaComponent:0.6];
        self.counter.textAlignment=NSTextAlignmentCenter;
        self.counter.text=@"Ожидание изображения…";
        [self addSubview:self.counter];
    }
    return self;
}
- (UIView*)hitTest:(CGPoint)point withEvent:(UIEvent*)event {
    UIView* hit=[super hitTest:point withEvent:event];
    return hit==self || hit==self.counter ? nil : hit;
}
- (void)layoutSubviews {
    [super layoutSubviews];
    CGRect safe=UIEdgeInsetsInsetRect(self.bounds,self.safeAreaInsets);
    const CGFloat size=MIN(50,MAX(42,(safe.size.width-28)/7));
    const CGFloat step=size+3;
    const CGFloat left=CGRectGetMinX(safe)+8;
    const CGFloat right=CGRectGetMaxX(safe)-8-3*step;
    const CGFloat top=CGRectGetMaxY(safe)-3*step-8;
    const CGPoint cells[]={{1,0},{0,1},{1,2},{2,1}};
    for (NSUInteger i=0;i<8;++i) {
        CGPoint cell=cells[i%4];
        self.buttons[i].frame=CGRectMake((i<4?left:right)+cell.x*step,top+cell.y*step,size,size);
    }
    self.buttons[8].frame=CGRectMake(left,top-size-6,size*1.3,size*.75);
    self.buttons[9].frame=CGRectMake(right+2*step-size*.3,top-size-6,size*1.3,size*.75);
    self.buttons[10].frame=CGRectMake(CGRectGetMidX(safe)-43,CGRectGetMaxY(safe)-40,86,32);
    self.counter.frame=CGRectMake(CGRectGetMidX(safe)-105,CGRectGetMinY(safe)+4,210,26);
}
- (void)buttonDown:(UIButton*)button {
    if(button.selected)return;
    button.selected=YES;
    button.backgroundColor=[UIColor.systemBlueColor colorWithAlphaComponent:0.65];
    executor_lsx4_apple_set_pad_button((uint32_t)button.tag,1);
}
- (void)buttonUp:(UIButton*)button {
    if(!button.selected)return;
    button.selected=NO;
    button.backgroundColor=[UIColor.blackColor colorWithAlphaComponent:0.42];
    executor_lsx4_apple_set_pad_button((uint32_t)button.tag,0);
}
- (void)releaseAllButtons {
    for(UIButton* button in self.buttons)[self buttonUp:button];
}
- (void)setInputReady:(BOOL)ready {
    if(!ready)[self releaseAllButtons];
    for(UIButton* button in self.buttons)button.enabled=ready;
}
@end
