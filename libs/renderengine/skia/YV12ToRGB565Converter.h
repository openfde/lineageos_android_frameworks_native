// YV12ToRGB565Converter.h
#pragma once

#include <android/hardware_buffer.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <memory>
#include <vector>

#include <ui/GraphicBuffer.h>

#include <list>
#include <pthread.h>
#include <semaphore.h>

class YV12ToRGB565Converter {
public:
    struct CovertInfo {
        android::sp<android::GraphicBuffer> hardwareBuffer;
        int width;
        int height;
        int stride;

        std::list<std::function<void()>> egl_work_queue;
        sem_t egl_go;
        sem_t egl_done;

        EGLDisplay mEglDisplay;
        EGLContext mEglContext;
        EGLSurface mEglSurface;

        GLuint mProgram;
        bool mInitialized;
    };

    YV12ToRGB565Converter();
    ~YV12ToRGB565Converter();

    // 初始化GL环境
    static bool initialize(CovertInfo* info);

    // 执行转换
    static bool convert_to_rgb565(CovertInfo* info, unsigned char* yv12Buffer,
                unsigned char* rgb565Buffer,
                int width, int height, int stride);

    static bool convert_to_rgb565_by_fd(CovertInfo* info, int yv12Fd, int rgbFd,
                                    int width, int height, int stride);

    // 清理资源
    void cleanup(CovertInfo* info);

    static void* egl_covert_loop(void* data);

private:
    // 创建和编译着色器
    static GLuint createProgram();
};