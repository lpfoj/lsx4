#import <Foundation/Foundation.h>
#ifdef __cplusplus
extern "C" {
#endif
void LSX4DiagArm(const char* directory,const char* operation);
void LSX4DiagDisarm(void);
BOOL LSX4TryImportPresentation(void (^operation)(void), NSError **error);
#ifdef __cplusplus
}
#endif
