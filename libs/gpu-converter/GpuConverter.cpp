
#include "GpuConverter.h"
#include <android/log.h>
#include <unistd.h>
#include <string>
#include <pthread.h>

#define ALIGN(x, mask) ( ((x) + (mask) - 1) & ~((mask) - 1) )

#define fourcc_code(a, b, c, d) ((uint32_t)(a) | ((uint32_t)(b) << 8) | \
                                 ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))
#define DRM_FORMAT_RGB565         fourcc_code('R', 'G', '1', '6') /* [15:0] R:G:B 5:6:5 little endian */
#define DRM_FORMAT_R8             fourcc_code('R', '8', ' ', ' ') /* [7:0] R */
#define DRM_FORMAT_BGRA8888       fourcc_code('B', 'A', '2', '4')  // BGRA 格式
#define DRM_FORMAT_RGBA8888       fourcc_code('R', 'A', '2', '4')
#define DRM_FORMAT_ARGB8888       fourcc_code('A', 'R', '2', '4')

#undef LOG_TAG
#define LOG_TAG "GpuConverter"

#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)

const char* eglStrError(EGLint err)
{
    switch (err){
        case EGL_SUCCESS:           return "EGL_SUCCESS";
        case EGL_NOT_INITIALIZED:   return "EGL_NOT_INITIALIZED";
        case EGL_BAD_ACCESS:        return "EGL_BAD_ACCESS";
        case EGL_BAD_ALLOC:         return "EGL_BAD_ALLOC";
        case EGL_BAD_ATTRIBUTE:     return "EGL_BAD_ATTRIBUTE";
        case EGL_BAD_CONFIG:        return "EGL_BAD_CONFIG";
        case EGL_BAD_CONTEXT:       return "EGL_BAD_CONTEXT";
        case EGL_BAD_CURRENT_SURFACE: return "EGL_BAD_CURRENT_SURFACE";
        case EGL_BAD_DISPLAY:       return "EGL_BAD_DISPLAY";
        case EGL_BAD_MATCH:         return "EGL_BAD_MATCH";
        case EGL_BAD_NATIVE_PIXMAP: return "EGL_BAD_NATIVE_PIXMAP";
        case EGL_BAD_NATIVE_WINDOW: return "EGL_BAD_NATIVE_WINDOW";
        case EGL_BAD_PARAMETER:     return "EGL_BAD_PARAMETER";
        case EGL_BAD_SURFACE:       return "EGL_BAD_SURFACE";
        case EGL_CONTEXT_LOST:      return "EGL_CONTEXT_LOST";
        default: return "UNKNOWN";
    }
}

GpuConverter::GpuConverter(ConvertInfo* info) {
    info->mEglDisplay = EGL_NO_DISPLAY;
    info->mEglContext = EGL_NO_CONTEXT;
    info->mEglSurface = EGL_NO_SURFACE;
    info->mProgram = 0;
    info->mInitialized = false;
    info->width = 0;
    info->height = 0;
    mConvertInfo = info;
    int err = pthread_create(&info->mGpuConvertThread, nullptr, this->eglConvertLoop, info);
}

GpuConverter::~GpuConverter() {
    mConvertInfo->mConvertThreadStop.store(true);
    mConvertInfo->queue_cv.notify_all();
    int err = pthread_join(mConvertInfo->mGpuConvertThread, NULL);
    cleanup(mConvertInfo);
}

GLuint GpuConverter::createProgram() {
    const char* vertex_shader_source =
        "attribute vec4 a_position;\n"
        "attribute vec2 a_texCoord;\n"
        "varying vec2 v_texCoord;\n"
        "void main() {\n"
        "    gl_Position = a_position;\n"
        "    v_texCoord = a_texCoord;\n"
        "}\n";

    const char* fragment_shader_source =
        "precision mediump float;\n"
        "varying vec2 v_texCoord;\n"
        "uniform sampler2D y_texture;\n"
        "uniform sampler2D u_texture;\n"
        "uniform sampler2D v_texture;\n"
        "void main() {\n"
        "    float y = texture2D(y_texture, v_texCoord).r;\n"
        "    float u = texture2D(u_texture, v_texCoord).r;\n"
        "    float v = texture2D(v_texture, v_texCoord).r;\n"
        "    float r = y + 1.402 * (v - 0.5);\n"
        "    float g = y - 0.344 * (u - 0.5) - 0.714 * (v - 0.5);\n"
        "    float b = y + 1.772 * (u - 0.5);\n"
        "    gl_FragColor = vec4(b, g, r, 1.0);\n"
        "}\n";

    GLuint vertex_shader = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vertex_shader, 1, &vertex_shader_source, NULL);
    glCompileShader(vertex_shader);
    GLint compile_status;
    glGetShaderiv(vertex_shader, GL_COMPILE_STATUS, &compile_status);
    if (!compile_status) {
        LOGE("Vertex shader compilation failed\n");
        return 0;
    }

    GLuint fragment_shader = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fragment_shader, 1, &fragment_shader_source, NULL);
    glCompileShader(fragment_shader);
    glGetShaderiv(fragment_shader, GL_COMPILE_STATUS, &compile_status);
    if (!compile_status) {
        LOGE("Fragment shader compilation failed\n");
        return 0;
    }

    GLuint program = glCreateProgram();
    glAttachShader(program, vertex_shader);
    glAttachShader(program, fragment_shader);
    glLinkProgram(program);
    glGetProgramiv(program, GL_LINK_STATUS, &compile_status);
    if (!compile_status) {
        LOGE("Program linking failed\n");
        return 0;
    }

    glDeleteShader(vertex_shader);
    glDeleteShader(fragment_shader);

    return program;
}

bool GpuConverter::initialize(ConvertInfo* info) {
    if (info->mInitialized) {
        return true;
    }

    if (info->width <= 0 || info->height <= 0) {
        return false;
    }

    LOGI("Initializing GPU converter %dx%d", info->width, info->height);

    // 初始化EGL
    info->mEglDisplay = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (info->mEglDisplay == EGL_NO_DISPLAY) {
        LOGE("Failed to get EGL display");
        return false;
    }

    if (!eglInitialize(info->mEglDisplay, NULL, NULL)) {
        LOGE("Failed to initialize EGL");
        return false;
    }

    // 选择配置
    const EGLint configAttribs[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_BLUE_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_RED_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_NONE
    };

    EGLConfig config;
    EGLint numConfigs;
    if (!eglChooseConfig(info->mEglDisplay, configAttribs, &config, 1, &numConfigs)) {
        LOGE("Failed to choose EGL config");
        return false;
    }

    if (numConfigs == 0) {
        LOGE("No suitable EGL config found :%s", eglStrError(eglGetError()));
        return false;
    }

    // 创建PBuffer表面
    const EGLint pbufferAttribs[] = {
        EGL_WIDTH, EGLint(info->width),
        EGL_HEIGHT, EGLint(info->height),
        EGL_NONE
    };

    info->mEglSurface = eglCreatePbufferSurface(info->mEglDisplay, config, pbufferAttribs);
    if (info->mEglSurface == EGL_NO_SURFACE) {
        LOGE("Failed to create EGL surface: 0x%x", eglGetError());
        return false;
    }

    // 创建上下文
    const EGLint contextAttribs[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_NONE
    };

    info->mEglContext = eglCreateContext(info->mEglDisplay, config, EGL_NO_CONTEXT, contextAttribs);
    if (info->mEglContext == EGL_NO_CONTEXT) {
        LOGE("Failed to create EGL context: 0x%x", eglGetError());
        return false;
    }

    if (!eglMakeCurrent(info->mEglDisplay, info->mEglSurface, info->mEglSurface, info->mEglContext)) {
        LOGE("Failed to make EGL current: 0x%x", eglGetError());
        return false;
    }

    // 创建着色器程序
    info->mProgram = createProgram();
    if (!info->mProgram) {
        LOGE("Failed to create shader program");
        return false;
    }

    // 检查GL错误
    GLenum error = glGetError();
    if (error != GL_NO_ERROR) {
        LOGW("OpenGL error after initialization: 0x%x", error);
        return false;
    }

    info->mInitialized = true;

    return true;
}


// 保存RGB文件
int saveRgbaToFile(const char* filename, const unsigned char* rgb, int size) {
    FILE* file = fopen(filename, "wb");
    if (!file) {
        LOGE("Failed to open output file");
        return -1;
    }

    size_t written = fwrite(rgb, sizeof(unsigned char), size, file);
    if ((int)written != size) {
        LOGE("Failed to write rgb data");
        fclose(file);
        return -1;
    }

    fclose(file);
    return 0;
}

bool GpuConverter::convertYv12ToRgba(ConvertInfo* info, unsigned char* yv12Buffer,
                                    unsigned char* rgbBuffer,
                                    int width, int height, int stride) {
    if (yv12Buffer == NULL || rgbBuffer == NULL
            || width <= 0 || height <= 0 || stride <= 0) {
        return false;
    }
    if (!info->mInitialized) {
        LOGD("convertYv12ToRgba Not initialized");
        info->width = width;
        info->height = height;
        initialize(info);
    } else if (width != info->width || height != info->height) {
        cleanup(info);
        info->width = width;
        info->height = height;
        LOGD("update EGL %dx%d", info->width, info->height);
        initialize(info);
    }

    int y_size = stride * height;
    int uv_size = (width/2) * (height/2);

    unsigned char* y_data = yv12Buffer;
    unsigned char* v_data = yv12Buffer + y_size;
    unsigned char* u_data = yv12Buffer + y_size + uv_size;

    GLuint fbo, color_tex;
    glGenFramebuffers(1, &fbo);
    glGenTextures(1, &color_tex);

    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glBindTexture(GL_TEXTURE_2D, color_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color_tex, 0);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        LOGE("Framebuffer not complete\n");
        return false;
    }

    glUseProgram(info->mProgram);
    GLuint y_loc = glGetUniformLocation(info->mProgram, "y_texture");
    GLuint u_loc = glGetUniformLocation(info->mProgram, "u_texture");
    GLuint v_loc = glGetUniformLocation(info->mProgram, "v_texture");
    glUniform1i(y_loc, 0);
    glUniform1i(u_loc, 1);
    glUniform1i(v_loc, 2);

    GLfloat vertices[] = {
        -1.0f, -1.0f, 0.0f, 0.0f,
         1.0f, -1.0f, 1.0f, 0.0f,
        -1.0f,  1.0f, 0.0f, 1.0f,
         1.0f,  1.0f, 1.0f, 1.0f
    };

    GLuint vbo;
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

    GLuint pos_loc = glGetAttribLocation(info->mProgram, "a_position");
    GLuint tex_loc = glGetAttribLocation(info->mProgram, "a_texCoord");
    glEnableVertexAttribArray(pos_loc);
    glVertexAttribPointer(pos_loc, 2, GL_FLOAT, GL_FALSE, 4*sizeof(GLfloat), (void*)0);
    glEnableVertexAttribArray(tex_loc);
    glVertexAttribPointer(tex_loc, 2, GL_FLOAT, GL_FALSE, 4*sizeof(GLfloat), (void*)(2*sizeof(GLfloat)));


    GLuint y_tex, u_tex, v_tex;
    glGenTextures(1, &y_tex);
    glGenTextures(1, &u_tex);
    glGenTextures(1, &v_tex);

    // Y平面纹理
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, y_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, width, height, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, y_data);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // V平面纹理
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, u_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, width/2, height/2,
                                0, GL_LUMINANCE, GL_UNSIGNED_BYTE, u_data);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // U平面纹理
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, v_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, width/2, height/2,
                                0, GL_LUMINANCE, GL_UNSIGNED_BYTE, v_data);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);


    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    glFinish();  // 确保所有渲染命令完成
    GLint fbo_status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (fbo_status != GL_FRAMEBUFFER_COMPLETE) {
        LOGE("Framebuffer not complete\n");
        return false;
    }

    unsigned char* rgb_data = rgbBuffer;
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, rgb_data);
    // save_rgb_file("/data/skia/rgba.bin", rgb_data, width, height, width*height*4);

    glBindTexture(GL_TEXTURE_2D, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    glDeleteTextures(1, &y_tex);
    glDeleteTextures(1, &u_tex);
    glDeleteTextures(1, &v_tex);
    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(1, &color_tex);
    glDeleteBuffers(1, &vbo);
    return true;
}


bool GpuConverter::convertYv12ToRgbaByFd(ConvertInfo* info, int yv12Fd, int rgbFd,
                                    int width, int height, int stride) {
    if (yv12Fd <= 0 || rgbFd <= 0
            || width <= 0 || height <= 0 || stride <= 0) {
        return false;
    }
    if (!info->mInitialized) {
        LOGD("convertYv12ToRgbaByFd Not initialized");
        info->width = width;
        info->height = height;
        initialize(info);
    } else if (width != info->width || height != info->height) {
        cleanup(info);
        info->width = width;
        info->height = height;
        LOGD("update EGL %dx%d", info->width, info->height);
        initialize(info);
    }

    EGLint output_attribs[] = {
        EGL_WIDTH,             width,
        EGL_HEIGHT,            height,
        EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_ARGB8888,
        EGL_DMA_BUF_PLANE0_FD_EXT, rgbFd,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
        EGL_DMA_BUF_PLANE0_PITCH_EXT, width*4,
        EGL_IMAGE_PRESERVED_KHR, EGL_TRUE,
        EGL_NONE
    };
    EGLImageKHR output_image = eglCreateImageKHR(
        info->mEglDisplay,
        EGL_NO_CONTEXT,
        EGL_LINUX_DMA_BUF_EXT,
        NULL,  // 无client buffer
        output_attribs
    );
    if (output_image == EGL_NO_IMAGE_KHR) {
        LOGE("Failed to create out EGLImage: error=%d, pitch:%d\n", eglGetError(), width*4);
        return -1;
    }

    int y_stride = stride;
    int v_stride = stride / 2; // V 平面的步长
    int u_stride = stride / 2; // U 平面的步长

    // 计算每个平面的偏移量
    int y_size = stride * height;
    int v_size = (stride / 2) * (height / 2);
    int y_offset = 0;
    int v_offset = y_size;
    int u_offset = v_offset + v_size;

    EGLint yAttribs[] = {
        EGL_WIDTH, width,
        EGL_HEIGHT, height,
        EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_R8, // Y平面是8位
        EGL_DMA_BUF_PLANE0_FD_EXT, yv12Fd,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, y_offset,
        EGL_DMA_BUF_PLANE0_PITCH_EXT, y_stride,
        EGL_NONE
    };
    EGLImageKHR y_image = eglCreateImageKHR(info->mEglDisplay, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, yAttribs);
    if (y_image == EGL_NO_IMAGE_KHR) {
        // 处理错误
        LOGE("Failed to create yEGLImage: error=%s\n", eglStrError(eglGetError()));
        return -1;
    }

    EGLint vAttribs[] = {
        EGL_WIDTH, width/2,
        EGL_HEIGHT, height/2,
        EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_R8, // V平面是8位
        EGL_DMA_BUF_PLANE0_FD_EXT, yv12Fd,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, v_offset,
        EGL_DMA_BUF_PLANE0_PITCH_EXT, v_stride,
        EGL_NONE
    };
    EGLImageKHR v_image = eglCreateImageKHR(info->mEglDisplay, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, vAttribs);
    if (v_image == EGL_NO_IMAGE_KHR) {
        // 处理错误
        LOGE("Failed to create vEGLImage: error=%s\n", eglStrError(eglGetError()));
        return -1;
    }

    EGLint uAttribs[] = {
        EGL_WIDTH, width/2,
        EGL_HEIGHT, height/2,
        EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_R8, // U平面是8位
        EGL_DMA_BUF_PLANE0_FD_EXT, yv12Fd,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, u_offset,
        EGL_DMA_BUF_PLANE0_PITCH_EXT, u_stride,
        EGL_NONE
    };
    EGLImageKHR u_image = eglCreateImageKHR(info->mEglDisplay, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, uAttribs);
    if (u_image == EGL_NO_IMAGE_KHR) {
        // 处理错误
        LOGE("Failed to create yEGLImage: error=%s\n", eglStrError(eglGetError()));
        return -1;
    }

    GLuint fbo, color_tex;
    glGenFramebuffers(1, &fbo);
    glGenTextures(1, &color_tex);

    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glBindTexture(GL_TEXTURE_2D, color_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, (GLeglImageOES)output_image);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color_tex, 0);

    // if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
    //     LOGE("Framebuffer not complete\n");
    //     return false;
    // }

    glUseProgram(info->mProgram);
    GLuint y_loc = glGetUniformLocation(info->mProgram, "y_texture");
    GLuint u_loc = glGetUniformLocation(info->mProgram, "u_texture");
    GLuint v_loc = glGetUniformLocation(info->mProgram, "v_texture");
    glUniform1i(y_loc, 0);
    glUniform1i(u_loc, 1);
    glUniform1i(v_loc, 2);

    GLfloat vertices[] = {
        -1.0f, -1.0f, 0.0f, 0.0f,
         1.0f, -1.0f, 1.0f, 0.0f,
        -1.0f,  1.0f, 0.0f, 1.0f,
         1.0f,  1.0f, 1.0f, 1.0f
    };

    GLuint vbo;
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

    GLuint pos_loc = glGetAttribLocation(info->mProgram, "a_position");
    GLuint tex_loc = glGetAttribLocation(info->mProgram, "a_texCoord");
    glEnableVertexAttribArray(pos_loc);
    glVertexAttribPointer(pos_loc, 2, GL_FLOAT, GL_FALSE, 4*sizeof(GLfloat), (void*)0);
    glEnableVertexAttribArray(tex_loc);
    glVertexAttribPointer(tex_loc, 2, GL_FLOAT, GL_FALSE, 4*sizeof(GLfloat), (void*)(2*sizeof(GLfloat)));

    GLuint y_tex, u_tex, v_tex;
    glGenTextures(1, &y_tex);
    glGenTextures(1, &u_tex);
    glGenTextures(1, &v_tex);

    // Y平面纹理
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, y_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, width, height, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, NULL);
    glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, (GLeglImageOES)y_image);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // V平面纹理
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, v_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, width/2, height/2,
                                0, GL_LUMINANCE, GL_UNSIGNED_BYTE, NULL);
    glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, (GLeglImageOES)v_image);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // U平面纹理
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, u_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, width/2, height/2,
                                0, GL_LUMINANCE, GL_UNSIGNED_BYTE, NULL);
    glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, (GLeglImageOES)u_image);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);


    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glFinish();
    // GLint fbo_status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    // if (fbo_status != GL_FRAMEBUFFER_COMPLETE) {
    //     LOGE("Framebuffer not complete\n");
    //     return false;
    // }
    // LOGD("FBO status: %x\n", fbo_status);

    // glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, rgb_data);
    // save_rgb_file("/data/skia/rgb.bin", rgb_data, width, height, width*height*4);

    glBindTexture(GL_TEXTURE_2D, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    eglDestroyImageKHR(info->mEglDisplay, output_image);
    eglDestroyImageKHR(info->mEglDisplay, y_image);
    eglDestroyImageKHR(info->mEglDisplay, v_image);
    eglDestroyImageKHR(info->mEglDisplay, u_image);

    glDeleteTextures(1, &y_tex);
    glDeleteTextures(1, &u_tex);
    glDeleteTextures(1, &v_tex);
    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(1, &color_tex);
    glDeleteBuffers(1, &vbo);

    return true;
}

void GpuConverter::cleanup(ConvertInfo* info) {
    if (!info->mInitialized) {
        return ;
    }

    ALOGD("Cleaning up GPU converter %dx%d",info->width, info->height);

    if (info->mProgram) {
        glDeleteProgram(info->mProgram);
        info->mProgram = 0;
    }


    if (info->mEglDisplay != EGL_NO_DISPLAY) {
        // 重置GL上下文
        eglMakeCurrent(info->mEglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

        if (info->mEglContext != EGL_NO_CONTEXT) {
            eglDestroyContext(info->mEglDisplay, info->mEglContext);
            info->mEglContext = EGL_NO_CONTEXT;
        }

        if (info->mEglSurface != EGL_NO_SURFACE) {
            eglDestroySurface(info->mEglDisplay, info->mEglSurface);
            info->mEglSurface = EGL_NO_SURFACE;
        }

        eglTerminate(info->mEglDisplay);
        info->mEglDisplay = EGL_NO_DISPLAY;
    }

    info->mInitialized = false;
}

void* GpuConverter::eglConvertLoop(void* data) {
    if (data == NULL) return NULL;
    struct ConvertInfo* info = (struct ConvertInfo*) data;
    // initialize(info);

    while (true) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(info->queue_mutex);
            info->queue_cv.wait(lock, [&]() {
                return !info->egl_work_queue.empty() || info->mConvertThreadStop.load();
            });

            if (info->mConvertThreadStop.load()) {
                info->done_cv.notify_one();
                break;
            }

            if (info->egl_work_queue.empty()) {
                continue;
            }

            task = std::move(info->egl_work_queue.front());
            info->egl_work_queue.pop_front();

            if (task) {
                task();
            }
        }

        {
            std::lock_guard<std::mutex> lock(info->queue_mutex);
            info->pending_tasks_count--;
        }
        info->done_cv.notify_one();
    }

    // cleanup(info);
    return NULL;
}

bool GpuConverter::gpuConvertYv12ToRgba(ConvertInfo* info,
                                        unsigned char* yv12Buffer,
                                        unsigned char* rgbBuffer,
                                        int width, int height, int stride)
{
    if (info->width == 0 || info->height == 0) {
        info->width = width;
        info->height = height;
    }

    auto task_done = std::make_shared<bool>(false);
    auto task = std::bind([=]() {
        this->convertYv12ToRgba(info, yv12Buffer, rgbBuffer, width, height, stride);
        *task_done = true;
    });

    {
        std::lock_guard<std::mutex> lock(info->queue_mutex);
        info->egl_work_queue.push_back(task);
        info->pending_tasks_count++;
    }
    info->queue_cv.notify_one();

    {
        std::unique_lock<std::mutex> lock(info->queue_mutex);
        info->done_cv.wait(lock, [&]() { return *task_done == true; });
    }

    return true;
}

bool GpuConverter::gpuConvertYv12ToRgbaByFd(ConvertInfo* info,
                                                    int yv12Fd, int rgbFd,
                                                    int width, int height, int stride)
{
    if (info->width == 0 || info->height == 0) {
        info->width = width;
        info->height = height;
    }

    auto task_done = std::make_shared<bool>(false);
    auto task = std::bind([=]() {
        this->convertYv12ToRgbaByFd(info, yv12Fd, rgbFd, width, height, stride);
        *task_done = true;
    });

    {
        std::lock_guard<std::mutex> lock(info->queue_mutex);
        info->egl_work_queue.push_back(task);
        info->pending_tasks_count++;
    }
    info->queue_cv.notify_one();

    {
        std::unique_lock<std::mutex> lock(info->queue_mutex);
        info->done_cv.wait(lock, [&]() { return *task_done == true; });
    }

	return true;
}