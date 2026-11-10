#include "smoke_runtime.h"
#include "vulkan_smoke.h"
#include <jni.h>
#include <android/native_window_jni.h>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace {
std::mutex gMutex;
recomp::bootstrap::SmokeRuntime gRuntime;
recomp::bootstrap::VulkanSmokeRenderer gRenderer(gRuntime);
bool gCreated = false;
std::string fromJava(JNIEnv* env, jstring value) {
    if (!value) return {};
    const char* chars = env->GetStringUTFChars(value, nullptr);
    if (!chars) return {};
    std::string result(chars);
    env->ReleaseStringUTFChars(value, chars);
    return result;
}
}
extern "C" JNIEXPORT jboolean JNICALL
Java_dev_recompbootstrap_smoke_NativeBridge_create(JNIEnv* env, jclass, jstring data, jstring cache) {
    std::lock_guard<std::mutex> lock(gMutex);
    if (gCreated) return JNI_TRUE;
    gCreated = gRuntime.initialize(fromJava(env, data), fromJava(env, cache));
    return gCreated ? JNI_TRUE : JNI_FALSE;
}
extern "C" JNIEXPORT jboolean JNICALL
Java_dev_recompbootstrap_smoke_NativeBridge_configureRgbaDiagnostic(JNIEnv* env, jclass, jbyteArray bytes, jboolean reject) {
    if (!bytes) return JNI_FALSE;
    const jsize length = env->GetArrayLength(bytes);
    if (length <= 0 || length > 128 * 1024 * 1024) return JNI_FALSE;
    std::vector<std::uint8_t> copy(static_cast<std::size_t>(length));
    env->GetByteArrayRegion(bytes, 0, length, reinterpret_cast<jbyte*>(copy.data()));
    if (env->ExceptionCheck()) return JNI_FALSE;
    std::lock_guard<std::mutex> lock(gMutex);
    return gRenderer.configureRgbaDiagnostic(std::move(copy), reject == JNI_TRUE) ? JNI_TRUE : JNI_FALSE;
}
extern "C" JNIEXPORT void JNICALL
Java_dev_recompbootstrap_smoke_NativeBridge_resume(JNIEnv*, jclass) {
    std::lock_guard<std::mutex> lock(gMutex); gRuntime.resume();
}
extern "C" JNIEXPORT void JNICALL
Java_dev_recompbootstrap_smoke_NativeBridge_pause(JNIEnv*, jclass) {
    std::lock_guard<std::mutex> lock(gMutex); gRuntime.suspend();
}
extern "C" JNIEXPORT void JNICALL
Java_dev_recompbootstrap_smoke_NativeBridge_destroy(JNIEnv*, jclass) {
    std::lock_guard<std::mutex> lock(gMutex); gRenderer.detachWindow(); gRuntime.shutdown(); gCreated = false;
}
extern "C" JNIEXPORT jboolean JNICALL
Java_dev_recompbootstrap_smoke_NativeBridge_surfaceCreated(JNIEnv* env, jclass, jobject surface, jint width, jint height) {
    if (!surface) return JNI_FALSE;
    ANativeWindow* window = ANativeWindow_fromSurface(env, surface);
    if (!window) return JNI_FALSE;
    std::lock_guard<std::mutex> lock(gMutex);
    if (!gCreated) { ANativeWindow_release(window); return JNI_FALSE; }
    return gRenderer.attachWindow(window, width, height) ? JNI_TRUE : JNI_FALSE;
}
extern "C" JNIEXPORT jboolean JNICALL
Java_dev_recompbootstrap_smoke_NativeBridge_surfaceChanged(JNIEnv*, jclass, jint width, jint height) {
    std::lock_guard<std::mutex> lock(gMutex);
    return gRenderer.resize(width, height) ? JNI_TRUE : JNI_FALSE;
}
extern "C" JNIEXPORT void JNICALL
Java_dev_recompbootstrap_smoke_NativeBridge_surfaceDestroyed(JNIEnv*, jclass) {
    std::lock_guard<std::mutex> lock(gMutex); gRenderer.detachWindow();
}
extern "C" JNIEXPORT jstring JNICALL
Java_dev_recompbootstrap_smoke_NativeBridge_describe(JNIEnv* env, jclass) {
    std::lock_guard<std::mutex> lock(gMutex);
    const auto report = gRuntime.describe("graphics-self-test");
    return env->NewStringUTF(report.c_str());
}
