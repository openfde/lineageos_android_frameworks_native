
#include "YV12ToRGB565Converter.h"
#include <android/log.h>
#include <unistd.h>
#include <string>

#define ALIGN(x, mask) ( ((x) + (mask) - 1) & ~((mask) - 1) )

#define fourcc_code(a, b, c, d) ((uint32_t)(a) | ((uint32_t)(b) << 8) | \
                                 ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))
#define DRM_FORMAT_RGB565       fourcc_code('R', 'G', '1', '6') /* [15:0] R:G:B 5:6:5 little endian */
#define DRM_FORMAT_R8             fourcc_code('R', '8', ' ', ' ') /* [7:0] R */

// #define LOG_TAG "YV12ToRGB565Converter"

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

YV12ToRGB565Converter::YV12ToRGB565Converter() {

}

YV12ToRGB565Converter::~YV12ToRGB565Converter() {
    // cleanup();
}


GLuint YV12ToRGB565Converter::createProgram() {
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
        "    gl_FragColor = vec4(r, g, b, 1.0);\n"
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

bool YV12ToRGB565Converter::initialize(CovertInfo* info) {
    if (info->mInitialized) {
        LOGI("Initialized GPU converter...");
        return true;
    }

    LOGI("Initializing GPU converter...");

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
        LOGE("No suitable EGL config found");
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
    LOGI("GPU converter initialized successfully");

    return true;
}


// 保存RGB文件
int save_rgb_file(const char* filename, const unsigned char* rgb, int width, int height, int size) {
    FILE* file = fopen(filename, "wb");
    if (!file) {
        LOGE("Failed to open output file");
        return -1;
    }

    size_t written = fwrite(rgb, sizeof(unsigned char), size, file);
    if (written != size) {
        LOGE("Failed to write rgb data");
        fclose(file);
        return -1;
    }

    fclose(file);
    return 0;
}

bool YV12ToRGB565Converter::convert_to_rgb565(CovertInfo* info, unsigned char* yv12Buffer,
                                    unsigned char* rgb565Buffer,
                                    int width, int height, int stride) {
    if (!info->mInitialized) {
        LOGE("Not initialized");
        return false;
    }

    int y_size = width * height;
    int uv_size = (width/2) * (height/2);

    unsigned char* y_data = yv12Buffer;
    unsigned char* v_data = yv12Buffer + y_size;
    unsigned char* u_data = yv12Buffer + y_size + uv_size;

    GLuint fbo, color_tex;
    glGenFramebuffers(1, &fbo);
    glGenTextures(1, &color_tex);

    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glBindTexture(GL_TEXTURE_2D, color_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB565, width, height, 0, GL_RGB, GL_UNSIGNED_BYTE, NULL);
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
    // GLint fbo_status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    // if (fbo_status != GL_FRAMEBUFFER_COMPLETE) {
    //     LOGE("Framebuffer not complete\n");
    //     return false;
    // }
    // LOGD("FBO status: %x\n", fbo_status);

    unsigned char* rgb_data = rgb565Buffer;
    glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, rgb_data);
    // save_rgb_file("/data/skia/rgb565.bin", rgb_data, width, height, width*height*2);

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


bool YV12ToRGB565Converter::convert_to_rgb565_by_fd(CovertInfo* info, int yv12Fd, int rgbFd,
                                    int width, int height, int stride) {
    if (!info->mInitialized) {
        LOGE("Not initialized");
        return false;
    }

    EGLint output_attribs[] = {
        EGL_WIDTH,             width,
        EGL_HEIGHT,            height,
        EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_RGB565, // 指定格式为 RGB565
        EGL_DMA_BUF_PLANE0_FD_EXT, rgbFd,              // Y 平面的 fd (这里只有一个平面)
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,           // 数据从缓冲区开头开始
        EGL_DMA_BUF_PLANE0_PITCH_EXT, width*2,       // 步长
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
        LOGE("Failed to create out EGLImage: error=%d\n", eglGetError());
        return -1;
    }

    int y_stride = width;
    int v_stride = width / 2; // V 平面的步长
    int u_stride = width / 2; // U 平面的步长

    // 计算每个平面的偏移量
    int y_size = width * height;
    int v_size = (width / 2) * (height / 2);
    int y_offset = 0;
    int u_offset = y_size;
    int v_offset = u_offset + v_size;

    EGLint yAttribs[] = {
        EGL_WIDTH, width,
        EGL_HEIGHT, height,
        EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_R8, // Y平面是8位
        EGL_DMA_BUF_PLANE0_FD_EXT, yv12Fd,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, y_offset,
        EGL_DMA_BUF_PLANE0_PITCH_EXT, y_stride,
        EGL_NONE
    };
    EGLImageKHR yImage = eglCreateImageKHR(info->mEglDisplay, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, yAttribs);
    if (yImage == EGL_NO_IMAGE_KHR) {
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
    EGLImageKHR vImage = eglCreateImageKHR(info->mEglDisplay, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, vAttribs);
    if (vImage == EGL_NO_IMAGE_KHR) {
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
    EGLImageKHR uImage = eglCreateImageKHR(info->mEglDisplay, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, uAttribs);
    if (uImage == EGL_NO_IMAGE_KHR) {
        // 处理错误
        LOGE("Failed to create yEGLImage: error=%s\n", eglStrError(eglGetError()));
        return -1;
    }

    GLuint fbo, color_tex;
    glGenFramebuffers(1, &fbo);
    glGenTextures(1, &color_tex);

    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glBindTexture(GL_TEXTURE_2D, color_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB565, width, height, 0, GL_RGB, GL_UNSIGNED_BYTE, NULL);
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
    glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, (GLeglImageOES)yImage);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // V平面纹理
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, v_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, width/2, height/2,
                                0, GL_LUMINANCE, GL_UNSIGNED_BYTE, NULL);
    glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, (GLeglImageOES)vImage);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // U平面纹理
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, u_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, width/2, height/2,
                                0, GL_LUMINANCE, GL_UNSIGNED_BYTE, NULL);
    glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, (GLeglImageOES)uImage);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);


    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glFinish();  // 确保所有渲染命令完成
    // GLint fbo_status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    // if (fbo_status != GL_FRAMEBUFFER_COMPLETE) {
    //     LOGE("Framebuffer not complete\n");
    //     return false;
    // }
    // LOGD("FBO status: %x\n", fbo_status);

    // glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, rgb_data);
    // save_rgb_file("/data/skia/rgb565.bin", rgb_data, width, height, width*height*2);

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

void YV12ToRGB565Converter::cleanup(CovertInfo* info) {
    LOGI("Cleaning up GPU converter");

    if (info->mInitialized) {
        if (info->mProgram) {
            glDeleteProgram(info->mProgram);
            info->mProgram = 0;
        }

        // 重置GL上下文
        eglMakeCurrent(info->mEglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    }

    if (info->mEglDisplay != EGL_NO_DISPLAY) {
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

void* YV12ToRGB565Converter::egl_covert_loop(void* data) {
    struct CovertInfo* info = (struct CovertInfo*) data;
    initialize(info);

    while (true) {
        sem_wait(&info->egl_go);
        for (auto const& f : info->egl_work_queue) {
            f();
        }
        info->egl_work_queue.clear();
        sem_post(&info->egl_done);
    }
    return NULL;
}