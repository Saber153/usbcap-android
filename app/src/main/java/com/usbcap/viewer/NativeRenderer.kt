package com.usbcap.viewer

import android.view.Surface

/**
 * JNI桥接 - Native OpenGL ES 渲染器
 * 负责 YUV/NV12/P010 → RGB 的 GPU 硬件加速转换和显示
 */
class NativeRenderer {

    companion object {
        init {
            System.loadLibrary("usbcap_native")
        }

        const val FORMAT_NV12 = 0
        const val FORMAT_NV21 = 1
        const val FORMAT_P010 = 2
    }

    /**
     * 初始化渲染器，绑定Surface
     */
    external fun nativeInit(surface: Surface)

    /**
     * 设置像素格式 (FORMAT_NV12 / FORMAT_NV21 / FORMAT_P010)
     */
    external fun nativeSetFormat(fmt: Int)

    /**
     * 渲染一帧YUV数据到屏幕
     * @param yData  Y平面数据
     * @param uvData UV平面数据 (NV12: UV交错, P010: 10bit UV)
     * @param width  图像宽度
     * @param height 图像高度
     * @param yStride  Y平面行字节数（可能>width due to alignment）
     * @param uvStride UV平面行字节数
     */
    external fun nativeRenderFrame(
        yData: ByteArray,
        uvData: ByteArray,
        width: Int,
        height: Int,
        yStride: Int,
        uvStride: Int
    )

    /**
     * 更新Surface尺寸
     */
    external fun nativeSetSurfaceSize(width: Int, height: Int)

    /**
     * 释放所有资源
     */
    external fun nativeDestroy()

    // 状态
    private var initialized = false

    fun init(surface: Surface) {
        nativeInit(surface)
        initialized = true
    }

    fun setFormat(fmt: Int) {
        if (initialized) nativeSetFormat(fmt)
    }

    fun render(yData: ByteArray, uvData: ByteArray, width: Int, height: Int, yStride: Int, uvStride: Int) {
        if (initialized) nativeRenderFrame(yData, uvData, width, height, yStride, uvStride)
    }

    fun setSurfaceSize(w: Int, h: Int) {
        if (initialized) nativeSetSurfaceSize(w, h)
    }

    fun destroy() {
        if (initialized) {
            nativeDestroy()
            initialized = false
        }
    }
}
