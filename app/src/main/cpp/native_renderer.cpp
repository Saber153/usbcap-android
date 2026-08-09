#include "native_renderer.h"
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <android/log.h>

#define LOG_TAG "NativeRenderer"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

static GLuint s_program = 0;
static GLint s_posLoc = -1, s_texLoc = -1;

static const char* kVert =
    "attribute vec2 aPos;\n"
    "attribute vec2 aTex;\n"
    "varying vec2 vTex;\n"
    "void main(){\n"
    "  gl_Position=vec4(aPos,0,1);\n"
    "  vTex=aTex;\n"
    "}\n";

static const char* kFrag =
    "precision mediump float;\n"
    "varying vec2 vTex;\n"
    "uniform sampler2D uTex;\n"
    "void main(){\n"
    "  gl_FragColor=texture2D(uTex,vTex);\n"
    "}\n";

static void initProgram() {
    if (s_program) return;
    GLuint vs = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vs, 1, &kVert, nullptr);
    glCompileShader(vs);
    GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fs, 1, &kFrag, nullptr);
    glCompileShader(fs);
    s_program = glCreateProgram();
    glAttachShader(s_program, vs);
    glAttachShader(s_program, fs);
    glLinkProgram(s_program);
    s_posLoc = glGetAttribLocation(s_program, "aPos");
    s_texLoc = glGetAttribLocation(s_program, "aTex");
    glDeleteShader(vs);
    glDeleteShader(fs);
}

EglState egl_init(ANativeWindow* window) {
    EglState egl{};
    egl.display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (egl.display == EGL_NO_DISPLAY) { LOGE("eglGetDisplay failed"); return egl; }
    EGLint major, minor;
    eglInitialize((EGLDisplay)egl.display, &major, &minor);
    EGLint cfg[] = {EGL_SURFACE_TYPE,EGL_WINDOW_BIT,EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_RENDERABLE_TYPE,EGL_OPENGL_ES2_BIT,EGL_NONE};
    EGLConfig config; EGLint n;
    eglChooseConfig((EGLDisplay)egl.display, cfg, &config, 1, &n);
    EGLint ctx[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    egl.context = eglCreateContext((EGLDisplay)egl.display, config, EGL_NO_CONTEXT, ctx);
    egl.surface = eglCreateWindowSurface((EGLDisplay)egl.display, config, window, nullptr);
    eglMakeCurrent((EGLDisplay)egl.display, (EGLSurface)egl.surface, (EGLSurface)egl.surface, (EGLContext)egl.context);
    initProgram();
    return egl;
}

void render_texture(unsigned int tex_id, int w, int h) {
    glViewport(0, 0, w, h);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(s_program);
    GLfloat pos[] = {-1,-1, 1,-1, -1,1, 1,1};
    GLfloat tex[] = {0,1, 1,1, 0,0, 1,0};
    glEnableVertexAttribArray(s_posLoc);
    glVertexAttribPointer(s_posLoc, 2, GL_FLOAT, GL_FALSE, 0, pos);
    glEnableVertexAttribArray(s_texLoc);
    glVertexAttribPointer(s_texLoc, 2, GL_FLOAT, GL_FALSE, 0, tex);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex_id);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableVertexAttribArray(s_posLoc);
    glDisableVertexAttribArray(s_texLoc);
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
    ANativeWindow* w = ANativeWindow_fromSurface(env, surface);
    if (!w) return 0;
    EglState* egl = new EglState(egl_init(w));
    ANativeWindow_release(w);
    return (long)egl;
}

JNIEXPORT void JNICALL
Java_com_example_usbcap_EglHelper_nativeRender(JNIEnv* env, jobject thiz, long h, jint texId, jint w, jint ht) {
    EglState* egl = (EglState*)h;
    if (egl) render_texture(texId, w, ht);
}

JNIEXPORT void JNICALL
Java_com_example_usbcap_EglHelper_nativeRelease(JNIEnv* env, jobject thiz, long h) {
    EglState* egl = (EglState*)h;
    if (egl) { egl_destroy(*egl); delete egl; }
}
