/*
 * Copyright (C) 2012 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

//#define LOG_NDEBUG 0
#define LOG_TAG "CpuConsumer"
//#define ATRACE_TAG ATRACE_TAG_GRAPHICS

#include <gui/CpuConsumer.h>

#include <gui/BufferItem.h>
#include <utils/Log.h>
#include <cutils/properties.h>
#include <android-base/logging.h>

#define CC_LOGV(x, ...) ALOGV("[%s] " x, mName.string(), ##__VA_ARGS__)
//#define CC_LOGD(x, ...) ALOGD("[%s] " x, mName.string(), ##__VA_ARGS__)
//#define CC_LOGI(x, ...) ALOGI("[%s] " x, mName.string(), ##__VA_ARGS__)
#define CC_LOGW(x, ...) ALOGW("[%s] " x, mName.string(), ##__VA_ARGS__)
#define CC_LOGE(x, ...) ALOGE("[%s] " x, mName.string(), ##__VA_ARGS__)

namespace android {

const GLfloat kPositionVertices[] = {
    -1.0f, 1.0f,
    -1.0f, -1.0f,
    1.0f, -1.0f,
    1.0f, 1.0f,
};

const GLfloat kYuvPositionVertices[] = {
    0.0f, 1.0f,
    0.0f, 0.0f,
    1.0f, 0.0f,
    1.0f, 1.0f,
};

static const char *eglStrError(EGLint err){
    switch (err){
        case EGL_SUCCESS:			return "EGL_SUCCESS";
        case EGL_NOT_INITIALIZED:	return "EGL_NOT_INITIALIZED";
        case EGL_BAD_ACCESS:		return "EGL_BAD_ACCESS";
        case EGL_BAD_ALLOC: 		return "EGL_BAD_ALLOC";
        case EGL_BAD_ATTRIBUTE: 	return "EGL_BAD_ATTRIBUTE";
        case EGL_BAD_CONFIG:		return "EGL_BAD_CONFIG";
        case EGL_BAD_CONTEXT:		return "EGL_BAD_CONTEXT";
        case EGL_BAD_CURRENT_SURFACE: return "EGL_BAD_CURRENT_SURFACE";
        case EGL_BAD_DISPLAY:		return "EGL_BAD_DISPLAY";
        case EGL_BAD_MATCH: 		return "EGL_BAD_MATCH";
        case EGL_BAD_NATIVE_PIXMAP: return "EGL_BAD_NATIVE_PIXMAP";
        case EGL_BAD_NATIVE_WINDOW: return "EGL_BAD_NATIVE_WINDOW";
        case EGL_BAD_PARAMETER: 	return "EGL_BAD_PARAMETER";
        case EGL_BAD_SURFACE:		return "EGL_BAD_SURFACE";
        case EGL_CONTEXT_LOST:		return "EGL_CONTEXT_LOST";
        default: return "UNKNOWN";
    }
}

static void checkEglError(const char* op, EGLBoolean returnVal = EGL_TRUE) {
    if (returnVal != EGL_TRUE) {
        ALOGE("%s() returned %d\n", op, returnVal);
    }

    for (EGLint error = eglGetError(); error != EGL_SUCCESS; error = eglGetError()) {
        ALOGE("after %s() eglError %s (0x%x)\n", op, eglStrError(error), error);
    }
}

static void checkGlError(const char* op) {
    for (GLint error = glGetError(); error; error = glGetError()) {
        ALOGE("after %s() glError (0x%x)\n", op, error);
    }
}

CpuConsumer::CpuConsumer(const sp<IGraphicBufferConsumer>& bq,
        size_t maxLockedBuffers, bool controlledByApp) :
    ConsumerBase(bq, controlledByApp),
    mMaxLockedBuffers(maxLockedBuffers),
    mCurrentLockedBuffers(0)
{
    // Create tracking entries for locked buffers
    mAcquiredBuffers.insertAt(0, maxLockedBuffers);

    mConsumer->setConsumerUsageBits(GRALLOC_USAGE_SW_READ_OFTEN);
    mConsumer->setMaxAcquiredBufferCount(static_cast<int32_t>(maxLockedBuffers));
}

CpuConsumer::~CpuConsumer() {
    CC_LOGE("~CpuConsumer");
    closeEgl();
}

static GLint createShader(GLenum type, const char* shaderSrc) {
    GLint success = 0;
    GLint logLength = 0;
    char infoLog[1024];

    GLint shader = glCreateShader(type);
    checkGlError("glCreateShader");
    glShaderSource(shader, 1, &shaderSrc, 0);
    checkGlError("glShaderSource");
    glCompileShader(shader);
    checkGlError("glCompileShader");
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    checkGlError("glGetShaderiv");
    if (!success) {
        glGetShaderInfoLog(shader, sizeof(infoLog), &logLength, infoLog);
        checkGlError("glGetShaderInfoLog");
        ALOGE("shader compilation failed:\n%s\n", infoLog);
    }
    return shader;
}

static GLint createProgram(const char* vs, const char* fs) {
    GLint success = 0;
    GLint logLength = 0;
    char infoLog[1024];

    GLint vertexShader = createShader(GL_VERTEX_SHADER, vs);
    GLint fragmentShader = createShader(GL_FRAGMENT_SHADER, fs);

    GLint program = glCreateProgram();
    checkGlError("glCreateProgram");
    glAttachShader(program, fragmentShader);
    checkGlError("glAttachShader");
    glAttachShader(program, vertexShader);
    checkGlError("glAttachShader");
    glLinkProgram(program);
    checkGlError("glLinkProgram");
    glGetProgramiv(program, GL_LINK_STATUS, &success);
    checkGlError("glGetProgramiv");
    if (!success) {
        glGetProgramInfoLog(program, sizeof(infoLog), &logLength, infoLog);
        checkGlError("glGetProgramInfoLog");
        ALOGE("Program linking failed:\n%s\n", infoLog);
    }

    glDeleteShader(vertexShader);
    checkGlError("glDeleteShader");
    glDeleteShader(fragmentShader);
    checkGlError("glDeleteShader");

    return program;
}


status_t CpuConsumer::initEgl(size_t width, size_t height, const bool isPossiblyYUV) {
    if (mIsInited) {
        return OK;
    }
    char property[PROPERTY_VALUE_MAX];
    if (!((property_get("ro.hardware.egl", property, "default") > 0) && (strcmp(property, "powervr") == 0))) {
        return INVALID_OPERATION;
    }

    mEglDisplay = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    checkEglError("eglGetDisplay");
    eglInitialize(mEglDisplay, NULL, NULL);
    checkEglError("eglInitialize");

    EGLConfig config;
    int num_config;
    EGLint dpy_attrs[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_NONE };
    eglChooseConfig(mEglDisplay, dpy_attrs, &config, 1, &num_config);
    checkEglError("eglChooseConfig");

    EGLint context_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    mEglContext = eglCreateContext(mEglDisplay, config,  EGL_NO_CONTEXT, context_attrs);
    checkEglError("eglChooseConfig");

    EGLint pbuf_attrs[] = { EGL_WIDTH, (EGLint)width, EGL_HEIGHT, (EGLint)height, EGL_NONE };
    mEglSurface = eglCreatePbufferSurface(mEglDisplay, config, pbuf_attrs);
    checkEglError("eglCreatePbufferSurface");

    eglMakeCurrent(mEglDisplay, mEglSurface, mEglSurface, mEglContext);
    checkEglError("eglMakeCurrent");

    mProgram = createProgram(isPossiblyYUV ? vertSourceYuv : vertSource, isPossiblyYUV ? fragSourceYuv : fragSource);
    glUseProgram(mProgram);
    checkGlError("glUseProgram");

    if (isPossiblyYUV) {
        mPosition = glGetAttribLocation(mProgram, "vPosition");
        checkGlError("glGetAttribLocation");

        mYuvPosition = glGetAttribLocation(mProgram, "vYuvTexCoords");
        checkGlError("glGetAttribLocation");

        mYuvTexSampler = glGetUniformLocation(mProgram, "yuvTexSampler");
        checkGlError("glGetUniformLocation");

        glVertexAttribPointer(mPosition, 2, GL_FLOAT, GL_FALSE, 0, kPositionVertices);
        checkGlError("glVertexAttribPointer");

        glEnableVertexAttribArray(mPosition);
        checkGlError("glEnableVertexAttribArray");

        glVertexAttribPointer(mYuvPosition, 2, GL_FLOAT, GL_FALSE, 0, kYuvPositionVertices);
        checkGlError("glVertexAttribPointer");

        glEnableVertexAttribArray(mYuvPosition);
        checkGlError("glEnableVertexAttribArray");

        glUniform1i(mYuvTexSampler, 0);
        checkGlError("glUniform1i");

        glViewport(0, 0, width, height);
        checkGlError("glViewport");
    }
    if (mShmData == nullptr) {
        mShmData = new GLubyte[width * height * 4];
        ALOGV("mShmData seted size: %u", (unsigned int)(width * height * 4));
    }
    mMemoryBuffer.insertAt(0, mMaxLockedBuffers);
    mIsInited = true;
    return OK;
}

void CpuConsumer::closeEgl() {
    ALOGV("closeEgl mIsInited: %d", mIsInited);
    if (mIsInited) {
        if (mShmData) {
            delete[] mShmData;
        }
        glDeleteProgram(mProgram);
        eglMakeCurrent(mEglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroySurface(mEglDisplay, mEglSurface);
        eglDestroyContext(mEglDisplay, mEglContext);
        eglTerminate(mEglDisplay);
    }
}

size_t CpuConsumer::findAcquiredBufferLocked(uintptr_t id) const {
    for (size_t i = 0; i < mMaxLockedBuffers; i++) {
        const auto& ab = mAcquiredBuffers[i];
        // note that this finds AcquiredBuffer::kUnusedId as well
        if (ab.mLockedBufferId == id) {
            return i;
        }
    }
    return mMaxLockedBuffers; // an invalid index
}

static uintptr_t getLockedBufferId(const CpuConsumer::LockedBuffer& buffer) {
    return reinterpret_cast<uintptr_t>(buffer.data);
}

static bool isPossiblyYUV(PixelFormat format) {
    switch (static_cast<int>(format)) {
        case HAL_PIXEL_FORMAT_RGBA_8888:
        case HAL_PIXEL_FORMAT_RGBX_8888:
        case HAL_PIXEL_FORMAT_RGBA_FP16:
        case HAL_PIXEL_FORMAT_RGBA_1010102:
        case HAL_PIXEL_FORMAT_RGB_888:
        case HAL_PIXEL_FORMAT_RGB_565:
        case HAL_PIXEL_FORMAT_BGRA_8888:
        case HAL_PIXEL_FORMAT_Y8:
        case HAL_PIXEL_FORMAT_Y16:
        case HAL_PIXEL_FORMAT_RAW16:
        case HAL_PIXEL_FORMAT_RAW10:
        case HAL_PIXEL_FORMAT_RAW_OPAQUE:
        case HAL_PIXEL_FORMAT_BLOB:
        case HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED:
            return false;

        case HAL_PIXEL_FORMAT_YV12:
        case HAL_PIXEL_FORMAT_YCbCr_420_888:
        case HAL_PIXEL_FORMAT_YCbCr_422_SP:
        case HAL_PIXEL_FORMAT_YCrCb_420_SP:
        case HAL_PIXEL_FORMAT_YCbCr_422_I:
        default:
            return true;
    }
}

__attribute__((no_sanitize("integer")))
static void ConvertRGB32ToPlanar(uint8_t *dstY, size_t dstStride, size_t dstVStride,
        const uint8_t *src, size_t width, size_t height, size_t srcStride, bool bgr) {
    CHECK((width & 1) == 0);
    CHECK((height & 1) == 0);
    (void)dstVStride;
    uint8_t *dstU = dstY + dstStride * height;

#ifdef SURFACE_IS_BGR32
    bgr = !bgr;
#endif

    const size_t redOffset   = bgr ? 2 : 0;
    const size_t greenOffset = 1;
    const size_t blueOffset  = bgr ? 0 : 2;

    for (size_t y = 0; y < height; ++y) {
        for (size_t x = 0; x < width; ++x) {
            unsigned red   = src[redOffset];
            unsigned green = src[greenOffset];
            unsigned blue  = src[blueOffset];

            // Using ITU-R BT.601-7 (03/2011)
            //   2.5.1: Ey'  = ( 0.299*R + 0.587*G + 0.114*B)
            //   2.5.2: ECr' = ( 0.701*R - 0.587*G - 0.114*B) / 1.402
            //          ECb' = (-0.299*R - 0.587*G + 0.886*B) / 1.772
            //   2.5.3: Y  = 219 * Ey'  +  16
            //          Cr = 224 * ECr' + 128
            //          Cb = 224 * ECb' + 128

            unsigned luma =
                ((red * 65 + green * 129 + blue * 25 + 128) >> 8) + 16;

            dstY[x] = luma;

            if ((x & 1) == 0 && (y & 1) == 0) {
                unsigned U =
                    ((-red * 38 - green * 74 + blue * 112 + 128) >> 8) + 128;

                unsigned V =
                    ((red * 112 - green * 94 - blue * 18 + 128) >> 8) + 128;

                dstU[x] = U;
                dstU[x + 1] = V;
            }
            src += 4;
        }

        if ((y & 1) == 0) {
            dstU += dstStride;
        }

        src += srcStride - 4 * width;
        dstY += dstStride;
    }
}

status_t CpuConsumer::lockBufferItem(const BufferItem& item, LockedBuffer* outBuffer) {
    android_ycbcr ycbcr = android_ycbcr();

    PixelFormat format = item.mGraphicBuffer->getPixelFormat();
    PixelFormat flexFormat = format;
    if (isPossiblyYUV(format)) {
        int fenceFd = item.mFence.get() ? item.mFence->dup() : -1;
        status_t err = item.mGraphicBuffer->lockAsyncYCbCr(GraphicBuffer::USAGE_SW_READ_OFTEN,
                                                           item.mCrop, &ycbcr, fenceFd);
        if (err == OK) {
            flexFormat = HAL_PIXEL_FORMAT_YCbCr_420_888;
            if (format != HAL_PIXEL_FORMAT_YCbCr_420_888) {
                CC_LOGV("locking buffer of format %#x as flex YUV", format);
            }
        } else if (format == HAL_PIXEL_FORMAT_YCbCr_420_888) {
            CC_LOGE("Unable to lock YCbCr buffer for CPU reading: %s (%d)", strerror(-err), err);
            return err;
        }
    }

    if (ycbcr.y != nullptr) {
        size_t width = item.mGraphicBuffer->getWidth();
        size_t height = item.mGraphicBuffer->getHeight();
        status_t status = initEgl(width, height, true);
        size_t lockedIdx = findAcquiredBufferLocked(AcquiredBuffer::kUnusedId);
        Vector<uint8_t>& mb = mMemoryBuffer.editItemAt(lockedIdx);
        if (status == OK) {
            ANativeWindowBuffer *buffer = item.mGraphicBuffer->getNativeBuffer();
            auto image = eglCreateImageKHR(mEglDisplay, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID, (EGLClientBuffer) buffer, nullptr);
            checkEglError("eglCreateImageKHR");

            GLuint texture;
            glGenTextures(1, &texture);
            checkGlError("glGenTextures");
            glBindTexture(GL_TEXTURE_EXTERNAL_OES, texture);
            checkGlError("glBindTexture");
            glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, (GLeglImageOES)image);
            checkGlError("glEGLImageTargetTexture2DOES");

            glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
            checkGlError("glDrawArrays");

            glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, mShmData);
            checkGlError("glReadPixels");

            mb.resize(width * height * 3 / 2);
            //maybe have other YUV format input todo!!!
            ConvertRGB32ToPlanar((uint8_t *)mb.editArray(), static_cast<uint32_t>(ycbcr.ystride), static_cast<uint32_t>(ycbcr.cstride),
                (const uint8_t *)mShmData, width, height, width * 4, false);

            glDeleteTextures(1, &texture);
            checkGlError("glDeleteTextures");
            eglDestroyImageKHR(mEglDisplay, image);
            checkEglError("eglDestroyImageKHR");
        }
        size_t stride = item.mGraphicBuffer->getStride();
        outBuffer->data = reinterpret_cast<uint8_t*>(((status != OK) ? ycbcr.y : mb.editArray()));
        outBuffer->stride = static_cast<uint32_t>(ycbcr.ystride);
        outBuffer->dataCb = reinterpret_cast<uint8_t*>(((status != OK) ? ycbcr.cb :
            mb.editArray() + stride * height));
        outBuffer->dataCr = reinterpret_cast<uint8_t*>(((status != OK) ? ycbcr.cr :
            mb.editArray() + stride * height + 1));
        outBuffer->chromaStride = static_cast<uint32_t>(ycbcr.cstride);
        outBuffer->chromaStep = static_cast<uint32_t>(ycbcr.chroma_step);
    } else {
        // not flexible YUV; try lockAsync
        void* bufferPointer = nullptr;
        int fenceFd = item.mFence.get() ? item.mFence->dup() : -1;
        status_t err = item.mGraphicBuffer->lockAsync(GraphicBuffer::USAGE_SW_READ_OFTEN,
                                                      item.mCrop, &bufferPointer, fenceFd);
        if (err != OK) {
            CC_LOGE("Unable to lock buffer for CPU reading: %s (%d)", strerror(-err), err);
            return err;
        }

        //egl transform todo!!!
        outBuffer->data = reinterpret_cast<uint8_t*>(bufferPointer);
        outBuffer->stride = item.mGraphicBuffer->getStride();
        outBuffer->dataCb = nullptr;
        outBuffer->dataCr = nullptr;
        outBuffer->chromaStride = 0;
        outBuffer->chromaStep = 0;
    }

    outBuffer->width = item.mGraphicBuffer->getWidth();
    outBuffer->height = item.mGraphicBuffer->getHeight();
    outBuffer->format = format;
    outBuffer->flexFormat = flexFormat;

    outBuffer->crop = item.mCrop;
    outBuffer->transform = item.mTransform;
    outBuffer->scalingMode = item.mScalingMode;
    outBuffer->timestamp = item.mTimestamp;
    outBuffer->dataSpace = item.mDataSpace;
    outBuffer->frameNumber = item.mFrameNumber;

    return OK;
}

status_t CpuConsumer::lockNextBuffer(LockedBuffer *nativeBuffer) {
    status_t err;

    if (!nativeBuffer) return BAD_VALUE;

    Mutex::Autolock _l(mMutex);

    if (mCurrentLockedBuffers == mMaxLockedBuffers) {
        CC_LOGW("Max buffers have been locked (%zd), cannot lock anymore.",
                mMaxLockedBuffers);
        return NOT_ENOUGH_DATA;
    }

    BufferItem b;
    err = acquireBufferLocked(&b, 0);
    if (err != OK) {
        if (err == BufferQueue::NO_BUFFER_AVAILABLE) {
            return BAD_VALUE;
        } else {
            CC_LOGE("Error acquiring buffer: %s (%d)", strerror(err), err);
            return err;
        }
    }

    if (b.mGraphicBuffer == nullptr) {
        b.mGraphicBuffer = mSlots[b.mSlot].mGraphicBuffer;
    }

    err = lockBufferItem(b, nativeBuffer);
    if (err != OK) {
        return err;
    }

    // find an unused AcquiredBuffer
    size_t lockedIdx = findAcquiredBufferLocked(AcquiredBuffer::kUnusedId);
    ALOG_ASSERT(lockedIdx < mMaxLockedBuffers);
    AcquiredBuffer& ab = mAcquiredBuffers.editItemAt(lockedIdx);

    ab.mSlot = b.mSlot;
    ab.mGraphicBuffer = b.mGraphicBuffer;
    ab.mLockedBufferId = getLockedBufferId(*nativeBuffer);

    mCurrentLockedBuffers++;

    return OK;
}

status_t CpuConsumer::unlockBuffer(const LockedBuffer &nativeBuffer) {
    Mutex::Autolock _l(mMutex);

    uintptr_t id = getLockedBufferId(nativeBuffer);
    size_t lockedIdx =
        (id != AcquiredBuffer::kUnusedId) ? findAcquiredBufferLocked(id) : mMaxLockedBuffers;
    if (lockedIdx == mMaxLockedBuffers) {
        CC_LOGE("%s: Can't find buffer to free", __FUNCTION__);
        return BAD_VALUE;
    }

    AcquiredBuffer& ab = mAcquiredBuffers.editItemAt(lockedIdx);
    if (mIsInited) {
        Vector<uint8_t>& mb = mMemoryBuffer.editItemAt(lockedIdx);
        if (ab.mLockedBufferId == reinterpret_cast<uintptr_t>(mb.array())) {
            mb.clear();
        } else {
            ALOGE("ab.mLockedBufferId != mb.array()\n");
        }
    }
    int fenceFd = -1;
    status_t err = ab.mGraphicBuffer->unlockAsync(&fenceFd);
    if (err != OK) {
        CC_LOGE("%s: Unable to unlock graphic buffer %zd", __FUNCTION__,
                lockedIdx);
        return err;
    }

    sp<Fence> fence(fenceFd >= 0 ? new Fence(fenceFd) : Fence::NO_FENCE);
    addReleaseFenceLocked(ab.mSlot, ab.mGraphicBuffer, fence);
    releaseBufferLocked(ab.mSlot, ab.mGraphicBuffer);

    ab.reset();

    mCurrentLockedBuffers--;

    return OK;
}

} // namespace android
