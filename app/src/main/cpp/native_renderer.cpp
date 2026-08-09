#include "native_renderer.h"
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <android/log.h>
#include <cstring>

#define LOG_TAG "NativeRenderer"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

EglState egl_init(ANativeWindow* window) {
    EglState egl{};
    egl.display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (egl.display == EGL_NO_DISPLAY) { LOGE("eglGetDisplay failed"); return egl; }

    EGLint major, minor;
    if (!eglInitialize((EGLDisplay)egl.display, &major, &minor)) { LOGE("eglInitialize failed"); return egl; }

    EGLint configAttribs[] = {EGL_SURFACE_TYPE,EGL_WINDOW_BIT,EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_ALPHA_SIZE,8,EGL_RENDERABLE_TYPE,EGL_OPENGL_ES2_BIT,EGL_NONE};
    EGLConfig config; EGLint numConfigs;
    eglChooseConfig((EGLDisplay)egl.display, configAttribs, &config, 1, &numConfigs);

    EGLint ctxAttr[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    egl.context = eglCreateContext((EGLDisplay)egl.display, config, EGL_NO_CONTEXT, ctxAttr);

    EGLint sAttr[] = {EGL_NONE};
    egl.surface = eglCreateWindowSurface((EGLDisplay)egl.display, config, window, sAttr);

    eglMakeCurrent((EGLDisplay)egl.display, (EGLSurface)egl.surface, (EGLSurface)egl.surface, (EGLContext)egl.context);
    LOGI("EGL initialized: %d.%d", major, minor);
    return egl;
}

void render_texture(unsigned int tex_id, int width, int height) {
    glViewport(0, 0, width, height);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, tex_id);
    static const GLfloat v[] = {-1,-1, 1,-1, -1,1, 1,1};
    static const GLfloat t[] = {0,1, 1,1, 0,0, 1,0};
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glVertexPointer(2, GL_FLOAT, 0, v);
    glTexCoordPointer(2, GL_FLOAT, 0, t);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableClientState(GL_VERTEX_ARRAY);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
}

void egl_destroy(EglState& state) {
    if (state.display) {
        eglMakeCurrent((EGLDisplay)state.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (state.surface) eglDestroySurface((EGLDisplay)state.display, (EGLSurface)state.surface);
        if (state.context) eglDestroyContext((EGLDisplay)state.display, (EGLContext)state.context);
        eglTerminate((EGLDisplay)state.display);
    }
    state = {};
}

JNIEXPORT long JNICALL
Java_com_example_usbcap_EglHelper_nativeInit(JNIEnv* env, jobject thiz, jobject surface) {
    ANativeWindow* window = ANativeWindow_fromSurface(env, surface);
    if (!window) return 0;
    EglState* egl = new EglState(egl_init(window));
    ANativeWindow_release(window);
    return reinterpret_cast<long>(egl);
}

JNIEXPORT void JNICALL
Java_com_example_usbcap_EglHelper_nativeRender(JNIEnv* env, jobject thiz, long handle, jint texId, jint width, jint height) {
    EglState* egl = reinterpret_cast<EglState*>(handle);
    if (egl) render_texture(texId, width, height);
}

JNIEXPORT void JNICALL
Java_com_example_usbcap_EglHelper_nativeRelease(JNIEnv* env, jobject thiz, long handle) {
    EglState* egl = reinterpret_cast<EglState*>(handle);
    if (egl) { egl_destroy(*egl); delete egl; }
}
