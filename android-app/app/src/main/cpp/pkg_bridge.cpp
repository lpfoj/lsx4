#include <jni.h>

#include <array>
#include <cstddef>
#include <string>

extern "C" int lsx4_extract_ps4_pkg(const char* input, const char* output,
                                     char* error_buffer, std::size_t error_capacity);
extern "C" int lsx4_extract_ps4_pkg_fd(int input_fd, const char* output,
                                        char* error_buffer, std::size_t error_capacity);

namespace {

class UtfChars final {
public:
    UtfChars(JNIEnv* env, jstring value) : env_(env), value_(value) {
        if (value_ != nullptr) {
            chars_ = env_->GetStringUTFChars(value_, nullptr);
        }
    }

    ~UtfChars() {
        if (chars_ != nullptr) {
            env_->ReleaseStringUTFChars(value_, chars_);
        }
    }

    const char* get() const { return chars_; }

private:
    JNIEnv* env_;
    jstring value_;
    const char* chars_ = nullptr;
};

}

static jstring ExtractPackage(JNIEnv* env, const char* package_path,
                              const char* destination_path) {
    if (package_path == nullptr || destination_path == nullptr) {
        return env->NewStringUTF("PKG path is unavailable");
    }

    std::array<char, 2048> error{};
    const int result = lsx4_extract_ps4_pkg(package_path, destination_path, error.data(),
                                            error.size());
    if (result == 0) {
        return env->NewStringUTF("");
    }
    if (error[0] == '\0') {
        return env->NewStringUTF("PKG/PFS extraction failed");
    }
    return env->NewStringUTF(error.data());
}

extern "C" JNIEXPORT jstring JNICALL
Java_app_lsx4_android_RuntimeBridge_extractPs4Pkg(JNIEnv* env, jclass,
                                                   jstring package_path,
                                                   jstring destination_path) {
    UtfChars package(env, package_path);
    UtfChars destination(env, destination_path);
    return ExtractPackage(env, package.get(), destination.get());
}

extern "C" JNIEXPORT jstring JNICALL
Java_app_lsx4_android_RuntimeBridge_extractPs4PkgFd(JNIEnv* env, jclass, jint package_fd,
                                                     jstring destination_path) {
    if (package_fd < 0) {
        return env->NewStringUTF("PKG descriptor is invalid");
    }
    UtfChars destination(env, destination_path);
    if (destination.get() == nullptr) {
        return env->NewStringUTF("PKG destination is unavailable");
    }
    std::array<char, 2048> error{};
    const int result = lsx4_extract_ps4_pkg_fd(package_fd, destination.get(), error.data(),
                                               error.size());
    if (result == 0) {
        return env->NewStringUTF("");
    }
    return env->NewStringUTF(error[0] == '\0' ? "PKG/PFS extraction failed" : error.data());
}
