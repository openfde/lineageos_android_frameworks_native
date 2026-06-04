// GpuConverter.h
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

#include <atomic>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <future>
#include <functional>
#include <memory>

#include <utils/RefBase.h>

class GpuConverter : public android::RefBase {
public:
    struct ConvertInfo {
        android::sp<android::GraphicBuffer> hardwareBuffer;
        int width;
        int height;
        int stride;

        std::deque<std::function<void()>> egl_work_queue;
        std::mutex queue_mutex;
        std::condition_variable queue_cv;
        std::condition_variable done_cv;
        int pending_tasks_count = 0;

        std::atomic<bool> mConvertThreadStop{false};
        pthread_t mGpuConvertThread;

        EGLDisplay mEglDisplay;
        EGLContext mEglContext;
        EGLSurface mEglSurface;

        GLuint mProgram;
        bool mInitialized;
    };

    GpuConverter(ConvertInfo* info);
    ~GpuConverter();

    bool gpuConvertYv12ToRgbaByFd(ConvertInfo* info,
                                                    int yv12Fd, int rgbFd,
                                                    int width, int height, int stride);
    bool gpuConvertYv12ToRgba(ConvertInfo* info,
                                            unsigned char* yv12Buffer,
                                            unsigned char* rgbBuffer,
                                            int width, int height, int stride);
    
    bool convertYv12ToRgba(ConvertInfo* info, unsigned char* yv12Buffer,
                unsigned char* rgbBuffer,
                int width, int height, int stride);
private:
    // 创建和编译着色器
    GLuint createProgram();

    // 初始化GL环境
    bool initialize(ConvertInfo* info);

    // 执行转换
    bool convertYv12ToRgbaByFd(ConvertInfo* info, int yv12Fd, int rgbFd,
                                    int width, int height, int stride);

    // 清理资源
    void cleanup(ConvertInfo* info);

    static void* eglConvertLoop(void* data);

    ConvertInfo* mConvertInfo;
};