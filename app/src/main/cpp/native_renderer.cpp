#include <jni.h>
#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <cstring>
#include <atomic>
#include <thread>
#include <chrono>

#define TAG "NativeRenderer"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

// ========== EGL 渲染器 ==========

struct EGLContext {
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLSurface surface = EGL_NO_SURFACE;
    EGLContext context = EGL_NO_CONTEXT;
    EGLConfig config = nullptr;

    bool initialized = false;
};

static EGLContext egl;
static ANativeWindow* window = nullptr;
static int surfaceWidth = 0;
static int surfaceHeight = 0;

// YUV->RGB shader
static GLuint programYuv = 0;
static GLuint texY = 0, texU = 0, texV = 0;
static GLint locTexY, locTexU, locTexV;

// P010/NV12 双模式
static bool isP010 = false;

// ========== Vertex Shader (全屏四边形) ==========
static const char* vertexShaderSrc = R"(
attribute vec4 aPosition;
attribute vec2 aTexCoord;
varying vec2 vTexCoord;
void main() {
    gl_Position = aPosition;
    vTexCoord = aTexCoord;
}
)";

// ========== Fragment Shader (NV12 YUV420 -> RGB) ==========
static const char* fragmentShaderNV12 = R"(
precision mediump float;
varying vec2 vTexCoord;
uniform sampler2D texY;
uniform sampler2D texUV;

void main() {
    float y = texture2D(texY, vTexCoord).r;
    vec2 uv = texture2D(texUV, vTexCoord).ra - 0.5;

    // BT.601
    float r = y + 1.402 * uv.y;
    float g = y - 0.344136 * uv.x - 0.714136 * uv.y;
    float b = y + 1.772 * uv.x;

    gl_FragColor = vec4(clamp(r, 0.0, 1.0), clamp(g, 0.0, 1.0), clamp(b, 0.0, 1.0), 1.0);
}
)";

// ========== Fragment Shader (P010 HDR -> SDR) ==========
static const char* fragmentShaderP010 = R"(
precision highp float;
varying vec2 vTexCoord;
uniform sampler2D texY;
uniform sampler2D texUV;

void main() {
    // P010: 10-bit YUV
    float y = texture2D(texY, vTexCoord).r * 256.0 / 1023.0;
    vec2 uv = texture2D(texUV, vTexCoord).ra;
    uv = uv * 256.0 / 1023.0 - 0.5;

    // BT.709 (HD)
    float r = y + 1.5748 * uv.y;
    float g = y - 0.1873 * uv.x - 0.4681 * uv.y;
    float b = y + 1.8556 * uv.x;

    // 简单色调映射
    vec3 color = vec3(clamp(r, 0.0, 1.0), clamp(g, 0.0, 1.0), clamp(b, 0.0, 1.0));
    color = color / (color + vec3(0.8));  // Reinhard

    gl_FragColor = vec4(color, 1.0);
}
)";

static const char* fragmentShaderNV21 = R"(
precision mediump float;
varying vec2 vTexCoord;
uniform sampler2D texY;
uniform sampler2D texUV;

void main() {
    float y = texture2D(texY, vTexCoord).r;
    vec2 uv = texture2D(texUV, vTexCoord).ar - 0.5;

    float r = y + 1.402 * uv.y;
    float g = y - 0.344136 * uv.x - 0.714136 * uv.y;
    float b = y + 1.772 * uv.x;

    gl_FragColor = vec4(clamp(r, 0.0, 1.0), clamp(g, 0.0, 1.0), clamp(b, 0.0, 1.0), 1.0);
}
)";

static GLuint currentFragmentShader = 0;

// ========== Shader 工具 ==========

static GLuint compileShader(GLenum type, const char* source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);

    GLint compiled;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (!compiled) {
        GLchar log[512];
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        LOGE("Shader compile error: %s", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

static GLuint createProgram(const char* vertSrc, const char* fragSrc) {
    GLuint vs = compileShader(GL_VERTEX_SHADER, vertSrc);
    GLuint fs = compileShader(GL_FRAGMENT_SHADER, fragSrc);
    if (!vs || !fs) return 0;

    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);

    GLint linked;
    glGetProgramiv(prog, GL_LINK_STATUS, &linked);
    if (!linked) {
        GLchar log[512];
        glGetProgramInfoLog(prog, sizeof(log), nullptr, log);
        LOGE("Program link error: %s", log);
        glDeleteProgram(prog);
        return 0;
    }

    glDeleteShader(vs);
    glDeleteShader(fs);
    return prog;
}

// ========== 初始化 EGL ==========

static bool initEGL(ANativeWindow* win) {
    egl.display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (egl.display == EGL_NO_DISPLAY) {
        LOGE("eglGetDisplay failed");
        return false;
    }

    EGLint major, minor;
    if (!eglInitialize(egl.display, &major, &minor)) {
        LOGE("eglInitialize failed");
        return false;
    }

    // 选择配置：最小延迟
    EGLint configAttribs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 0,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_NONE
    };

    EGLint numConfigs;
    eglChooseConfig(egl.display, configAttribs, &egl.config, 1, &numConfigs);
    if (numConfigs == 0) {
        LOGE("No suitable EGL config");
        return false;
    }

    // 创建窗口Surface
    EGLint surfaceAttribs[] = { EGL_NONE };
    egl.surface = eglCreateWindowSurface(egl.display, egl.config, win, surfaceAttribs);
    if (egl.surface == EGL_NO_SURFACE) {
        LOGE("eglCreateWindowSurface failed");
        return false;
    }

    // 创建ES2 Context
    EGLint contextAttribs[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_NONE
    };
    egl.context = eglCreateContext(egl.display, egl.config, EGL_NO_CONTEXT, contextAttribs);
    if (egl.context == EGL_NO_CONTEXT) {
        LOGE("eglCreateContext failed");
        return false;
    }

    if (!eglMakeCurrent(egl.display, egl.surface, egl.surface, egl.context)) {
        LOGE("eglMakeCurrent failed");
        return false;
    }

    // 关键：设置SWAP间隔为0（无VSync，最低延迟）
    eglSwapInterval(egl.display, 0);

    LOGI("EGL initialized: %d.%d", major, minor);
    egl.initialized = true;
    return true;
}

static void cleanupEGL() {
    if (egl.display != EGL_NO_DISPLAY) {
        eglMakeCurrent(egl.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (egl.context != EGL_NO_CONTEXT) eglDestroyContext(egl.display, egl.context);
        if (egl.surface != EGL_NO_SURFACE) eglDestroySurface(egl.display, egl.surface);
        eglTerminate(egl.display);
    }
    egl = {};
}

// ========== JNI 接口 ==========

extern "C" {

JNIEXPORT void JNICALL
Java_com_usbcap_viewer_NativeRenderer_nativeInit(JNIEnv* env, jobject thiz, jobject surface) {
    window = ANativeWindow_fromSurface(env, surface);
    if (!window) {
        LOGE("Failed to get native window");
        return;
    }

    ANativeWindow_setBuffersGeometry(window, 0, 0, WINDOW_FORMAT_RGBX_8888);

    if (!initEGL(window)) {
        LOGE("EGL init failed");
        return;
    }

    // 创建着色器程序
    programYuv = createProgram(vertexShaderSrc, fragmentShaderNV12);
    if (!programYuv) {
        LOGE("Failed to create YUV program");
        return;
    }

    locTexY = glGetUniformLocation(programYuv, "texY");
    locTexU = glGetUniformLocation(programYuv, "texUV");

    // 创建纹理
    glGenTextures(1, &texY);
    glGenTextures(1, &texU);

    glBindTexture(GL_TEXTURE_2D, texY);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glBindTexture(GL_TEXTURE_2D, texU);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    LOGI("Native renderer initialized");
}

JNIEXPORT void JNICALL
Java_com_usbcap_viewer_NativeRenderer_nativeSetFormat(JNIEnv* env, jobject thiz, jint fmt) {
    // 0=NV12, 1=NV21, 2=P010
    if (fmt == 0) {
        currentFragmentShader = 0; // 使用默认NV12
        isP010 = false;
    } else if (fmt == 1) {
        currentFragmentShader = 0; // NV21在shader里切换
        isP010 = false;
    } else if (fmt == 2) {
        currentFragmentShader = 0;
        isP010 = true;
    }

    // 重新编译shader
    if (programYuv) glDeleteProgram(programYuv);

    if (isP010) {
        programYuv = createProgram(vertexShaderSrc, fragmentShaderP010);
    } else if (fmt == 1) {
        programYuv = createProgram(vertexShaderSrc, fragmentShaderNV21);
    } else {
        programYuv = createProgram(vertexShaderSrc, fragmentShaderNV12);
    }

    locTexY = glGetUniformLocation(programYuv, "texY");
    locTexU = glGetUniformLocation(programYuv, "texUV");

    LOGI("Format set: %d, P010=%d", fmt, isP010 ? 1 : 0);
}

JNIEXPORT void JNICALL
Java_com_usbcap_viewer_NativeRenderer_nativeRenderFrame(
    JNIEnv* env, jobject thiz,
    jbyteArray yData, jbyteArray uvData,
    jint width, jint height, jint yStride, jint uvStride
) {
    if (!egl.initialized || !programYuv) return;

    auto t0 = std::chrono::steady_clock::now();

    jbyte* yBuf = env->GetByteArrayElements(yData, nullptr);
    jbyte* uvBuf = env->GetByteArrayElements(uvData, nullptr);

    glViewport(0, 0, surfaceWidth, surfaceHeight);

    // 上传Y平面
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texY);
    if (yStride == width) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, width, height, 0,
                     GL_LUMINANCE, GL_UNSIGNED_BYTE, yBuf);
    } else {
        glPixelStorei(GL_UNPACK_ROW_LENGTH, yStride);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, width, height, 0,
                     GL_LUMINANCE, GL_UNSIGNED_BYTE, yBuf);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    }

    // 上传UV平面 (NV12: UV交错, 高度=height/2)
    int uvHeight = height / 2;
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, texU);
    if (isP010) {
        if (uvStride == width * 2) {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE_ALPHA, width, uvHeight, 0,
                         GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, uvBuf);
        } else {
            glPixelStorei(GL_UNPACK_ROW_LENGTH, uvStride / 2);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE_ALPHA, width, uvHeight, 0,
                         GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, uvBuf);
            glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        }
    } else {
        if (uvStride == width) {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE_ALPHA, width / 2, uvHeight, 0,
                         GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, uvBuf);
        } else {
            glPixelStorei(GL_UNPACK_ROW_LENGTH, uvStride / 2);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE_ALPHA, width / 2, uvHeight, 0,
                         GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, uvBuf);
            glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        }
    }

    env->ReleaseByteArrayElements(yData, yBuf, JNI_ABORT);
    env->ReleaseByteArrayElements(uvData, uvBuf, JNI_ABORT);

    // 绘制全屏四边形
    glUseProgram(programYuv);
    glUniform1i(locTexY, 0);
    glUniform1i(locTexU, 1);

    // 顶点数据: 位置 + 纹理坐标
    static const GLfloat vertices[] = {
        // x,    y,    u,   v
        -1.0f, -1.0f, 0.0f, 1.0f,
         1.0f, -1.0f, 1.0f, 1.0f,
        -1.0f,  1.0f, 0.0f, 0.0f,
         1.0f,  1.0f, 1.0f, 0.0f,
    };

    GLint posLoc = glGetAttribLocation(programYuv, "aPosition");
    GLint texLoc = glGetAttribLocation(programYuv, "aTexCoord");

    glEnableVertexAttribArray(posLoc);
    glVertexAttribPointer(posLoc, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), vertices);
    glEnableVertexAttribArray(texLoc);
    glVertexAttribPointer(texLoc, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), vertices + 2);

    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    glDisableVertexAttribArray(posLoc);
    glDisableVertexAttribArray(texLoc);

    // 提交到屏幕（双缓冲，0延迟swap）
    eglSwapBuffers(egl.display, egl.surface);

    auto t1 = std::chrono::steady_clock::now();
    float ms = std::chrono::duration<float, std::milli>(t1 - t0).count();
    LOGI("Render: %.1fms (%dx%d)", ms, width, height);
}

JNIEXPORT void JNICALL
Java_com_usbcap_viewer_NativeRenderer_nativeSetSurfaceSize(JNIEnv* env, jobject thiz, jint w, jint h) {
    surfaceWidth = w;
    surfaceHeight = h;
    LOGI("Surface size: %dx%d", w, h);
}

JNIEXPORT void JNICALL
Java_com_usbcap_viewer_NativeRenderer_nativeDestroy(JNIEnv* env, jobject thiz) {
    if (programYuv) {
        glDeleteProgram(programYuv);
        programYuv = 0;
    }
    if (texY) { glDeleteTextures(1, &texY); texY = 0; }
    if (texU) { glDeleteTextures(1, &texU); texU = 0; }

    cleanupEGL();

    if (window) {
        ANativeWindow_release(window);
        window = nullptr;
    }

    LOGI("Native renderer destroyed");
}

} // extern "C"
