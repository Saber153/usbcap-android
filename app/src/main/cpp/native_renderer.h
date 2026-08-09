#pragma once

#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <jni.h>

struct EglState {
    void* display;
    void* surface;
    void* context;
};

EglState egl_init(ANativeWindow* window);
void render_texture(unsigned int tex_id, int width, int height);
void egl_destroy(EglState& state);

#ifdef __cplusplus
extern "C" {
#endif

JNIEXPORT long JNICALL
Java_com_example_usbcap_EglHelper_nativeInit(JNIEnv* env, jobject thiz, jobject surface);

JNIEXPORT void JNICALL
Java_com_example_usbcap_EglHelper_nativeRender(JNIEnv* env, jobject thiz, long handle, jint texId, jint width, jint height);

JNIEXPORT void JNICALL
Java_com_example_usbcap_EglHelper_nativeRelease(JNIEnv* env, jobject thiz, long handle);

#ifdef __cplusplus
}
#endif
