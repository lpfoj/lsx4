// SPDX-License-Identifier: GPL-2.0-or-later
#import "LSX4CrashCapture.h"
// Local UIKit exception boundary; no crash handlers or networking.
BOOL LSX4TryImportPresentation(void (^operation)(void), NSError **error) {
    @try {operation();return YES;}
    @catch(NSException* exception) {
        if(error)*error=[NSError errorWithDomain:@"LSX4PickerPresentation" code:1
            userInfo:@{NSLocalizedDescriptionKey:exception.reason ?: @"The file picker could not be opened."}];
        return NO;
    }
}
void LSX4DiagArm(const char*,const char*) {}
void LSX4DiagDisarm(void) {}
