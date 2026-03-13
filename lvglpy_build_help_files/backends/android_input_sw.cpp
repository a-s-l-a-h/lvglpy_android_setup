/*
 * backends/android_input_sw.cpp
 *
 * Android Software Render backend for lvglpy.
 */

#include "backend_base.h"
#include "lvgl/lvgl.h"

#include <jni.h>
#include <android/log.h>
#include <GLES2/gl2.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>

#define TAG  "lvglpy_android"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

/* ═══════════════════════════════════════════════════════════════
   Display / GL state
═══════════════════════════════════════════════════════════════ */
static int           g_w    = 0, g_h = 0;
static uint8_t      *g_fb   = NULL;
static lv_color_t   *g_buf1 = NULL;
static lv_color_t   *g_buf2 = NULL;
static lv_display_t *g_disp = NULL;

static GLuint g_tex  = 0;
static GLuint g_prog = 0;
static GLuint g_vbo  = 0;

static bool g_lv_inited = false; // Prevents duplicated init on resume

/* ═══════════════════════════════════════════════════════════════
   Touch queue
═══════════════════════════════════════════════════════════════ */
#define TOUCH_Q 64
typedef struct { int32_t x, y, pressed; } TouchEvent;

static TouchEvent      g_tq[TOUCH_Q];
static int             g_tqh = 0, g_tqt = 0;
static pthread_mutex_t g_tmx = PTHREAD_MUTEX_INITIALIZER;
static int32_t         g_tx  = 0, g_ty = 0, g_tp = 0;

/* ═══════════════════════════════════════════════════════════════
   Text / key queue
═══════════════════════════════════════════════════════════════ */
#define TEXT_BUF 1024
static char            g_textbuf[TEXT_BUF];
static int             g_textlen         = 0;
static int             g_backspace_count = 0;
static pthread_mutex_t g_kmx = PTHREAD_MUTEX_INITIALIZER;

static lv_indev_t *g_kbd_indev = NULL;
static lv_group_t *g_kbd_group = NULL;

/* ═══════════════════════════════════════════════════════════════
   GL helpers
═══════════════════════════════════════════════════════════════ */
static const char *VS =
    "attribute vec2 p; attribute vec2 u; varying vec2 v;\n"
    "void main(){ v=u; gl_Position=vec4(p,0,1); }\n";
static const char *FS =
    "precision mediump float; uniform sampler2D t; varying vec2 v;\n"
    "void main(){ gl_FragColor=texture2D(t,v); }\n";

static GLuint mk_shader(GLenum type, const char *src){
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    return s;
}
static GLuint mk_prog_gl(void){
    GLuint p  = glCreateProgram();
    GLuint vs = mk_shader(GL_VERTEX_SHADER,   VS);
    GLuint fs = mk_shader(GL_FRAGMENT_SHADER, FS);
    glAttachShader(p, vs); glAttachShader(p, fs);
    glLinkProgram(p);
    glDeleteShader(vs); glDeleteShader(fs);
    return p;
}

/* ═══════════════════════════════════════════════════════════════
   LVGL flush
═══════════════════════════════════════════════════════════════ */
static void flush_cb(lv_display_t *d, const lv_area_t *a, uint8_t *map){
    int32_t rw = a->x2 - a->x1 + 1;
    int32_t rh = a->y2 - a->y1 + 1;
    for(int32_t row = 0; row < rh; row++){
        uint8_t *src = map + row * rw * 4;
        uint8_t *dst = g_fb + ((a->y1 + row) * g_w + a->x1) * 4;
        for(int32_t col = 0; col < rw; col++){
            dst[0] = src[2]; dst[1] = src[1];
            dst[2] = src[0]; dst[3] = src[3];
            src += 4; dst += 4;
        }
    }
    lv_display_flush_ready(d);
}

/* ═══════════════════════════════════════════════════════════════
   LVGL indev callbacks
═══════════════════════════════════════════════════════════════ */
static void touch_cb(lv_indev_t *indev, lv_indev_data_t *data){
    pthread_mutex_lock(&g_tmx);
    // FIX: Process ONE touch event per frame so LVGL correctly registers fast DOWN->UP clicks
    if(g_tqh != g_tqt){
        TouchEvent *e = &g_tq[g_tqh];
        g_tx = e->x; g_ty = e->y; g_tp = e->pressed;
        g_tqh = (g_tqh + 1) % TOUCH_Q;
    }
    pthread_mutex_unlock(&g_tmx);
    data->point.x = g_tx;
    data->point.y = g_ty;
    data->state   = g_tp ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

static void keyboard_cb(lv_indev_t *indev, lv_indev_data_t *data){
    (void)indev;
    data->state = LV_INDEV_STATE_RELEASED;
}

/* ═══════════════════════════════════════════════════════════════
   drain_text_queue
═══════════════════════════════════════════════════════════════ */
static void drain_text_queue(void){
    if(!g_kbd_group) return;
    lv_obj_t *ta = lv_group_get_focused(g_kbd_group);
    if(!ta) return;

    pthread_mutex_lock(&g_kmx);
    int  bs  = g_backspace_count;
    g_backspace_count = 0;
    char tmp[TEXT_BUF];
    int  len = g_textlen;
    if(len > 0){
        memcpy(tmp, g_textbuf, len);
        tmp[len] = '\0';
        g_textlen = 0;
    }
    pthread_mutex_unlock(&g_kmx);

    for(int i = 0; i < bs; i++)
        lv_textarea_delete_char(ta);
    if(len > 0)
        lv_textarea_add_text(ta, tmp);
}

/* ═══════════════════════════════════════════════════════════════
   Push helpers
═══════════════════════════════════════════════════════════════ */
static void push_touch(int32_t x, int32_t y, int pressed){
    pthread_mutex_lock(&g_tmx);
    int next = (g_tqt + 1) % TOUCH_Q;
    if(next != g_tqh){
        g_tq[g_tqt] = TouchEvent{x, y, pressed};
        g_tqt = next;
    }
    pthread_mutex_unlock(&g_tmx);
}

static void push_char(uint32_t cp){
    char utf8[5] = {0}; int bytes = 0;
    if(cp < 0x80){
        utf8[0] = (char)cp; bytes = 1;
    } else if(cp < 0x800){
        utf8[0] = (char)(0xC0|(cp>>6));
        utf8[1] = (char)(0x80|(cp&0x3F));
        bytes = 2;
    } else if(cp < 0x10000){
        utf8[0] = (char)(0xE0|(cp>>12));
        utf8[1] = (char)(0x80|((cp>>6)&0x3F));
        utf8[2] = (char)(0x80|(cp&0x3F));
        bytes = 3;
    } else {
        utf8[0] = (char)(0xF0|(cp>>18));
        utf8[1] = (char)(0x80|((cp>>12)&0x3F));
        utf8[2] = (char)(0x80|((cp>>6)&0x3F));
        utf8[3] = (char)(0x80|(cp&0x3F));
        bytes = 4;
    }
    pthread_mutex_lock(&g_kmx);
    if(g_textlen + bytes < TEXT_BUF - 1){
        memcpy(g_textbuf + g_textlen, utf8, bytes);
        g_textlen += bytes;
    }
    pthread_mutex_unlock(&g_kmx);
}

static uint32_t now_ms(void){
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

/* ═══════════════════════════════════════════════════════════════
   backend_base.h interface
═══════════════════════════════════════════════════════════════ */
void lvgl_py_display_init(int width, int height, bool resizable){
    (void)width; (void)height; (void)resizable;
    LOGI("lvgl_py_display_init: Android-SW");
}

// ---> THIS IS THE ONLY NEW FUNCTION ADDED // and not sure needed or not debugging the logic<---
void lvgl_py_run(void) {
    LOGI("lv.run() called by Python -> Returning immediately (Handled by Android GL)");
}

const char *lvgl_py_backend_name(void){
    return "Android-SW";
}

/* ═══════════════════════════════════════════════════════════════
   JNI implementations
═══════════════════════════════════════════════════════════════ */

/* FIX: Handle context creation (app startup OR background resume) */
static void impl_surface_created(JNIEnv *env, jobject obj){
    (void)env; (void)obj;
    
    // Always recreate GL Shaders/Textures because EGL Context is new here
    if (g_prog != 0) {
        glDeleteProgram(g_prog);
        glDeleteTextures(1, &g_tex);
        glDeleteBuffers(1, &g_vbo);
    }
    
    g_prog = mk_prog_gl();
    float v[] = { -1,-1,0,1, 1,-1,1,1, -1,1,0,0, 1,1,1,0 };
    
    glGenBuffers(1, &g_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(v), v, GL_STATIC_DRAW);
    
    glGenTextures(1, &g_tex);
    glBindTexture(GL_TEXTURE_2D, g_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,     GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,     GL_CLAMP_TO_EDGE);
    
    LOGI("impl_surface_created: GL objects recreated");
}

static void impl_native_init(JNIEnv *env, jobject obj, jint w, jint h){
    (void)env; (void)obj;

    g_w = w; g_h = h;
    glViewport(0, 0, w, h);

    free(g_fb);
    g_fb = (uint8_t *)calloc((size_t)(w * h * 4), 1);

    glBindTexture(GL_TEXTURE_2D, g_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, g_fb);

    size_t buf_px = (size_t)(w * (h / 10));

    // FIX: Only initialize LVGL internals once
    if(!g_lv_inited){
        lv_init();

        g_buf1 = (lv_color_t *)malloc(buf_px * sizeof(lv_color_t));
        g_buf2 = (lv_color_t *)malloc(buf_px * sizeof(lv_color_t));

        g_disp = lv_display_create(w, h);
        lv_display_set_flush_cb(g_disp, flush_cb);
        lv_display_set_buffers(g_disp, g_buf1, g_buf2,
                               buf_px * sizeof(lv_color_t),
                               LV_DISPLAY_RENDER_MODE_PARTIAL);

        lv_indev_t *touch = lv_indev_create();
        lv_indev_set_type(touch, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(touch, touch_cb);

        g_kbd_indev = lv_indev_create();
        lv_indev_set_type(g_kbd_indev, LV_INDEV_TYPE_KEYPAD);
        lv_indev_set_read_cb(g_kbd_indev, keyboard_cb);

        g_kbd_group = lv_group_create();
        lv_indev_set_group(g_kbd_indev, g_kbd_group);

        g_lv_inited = true;
        LOGI("impl_native_init: LVGL init %dx%d", w, h);
    } else {
        // Safe resize logic if returning from background or rotating
        lv_display_set_resolution(g_disp, w, h);
        
        free(g_buf1); free(g_buf2);
        g_buf1 = (lv_color_t *)malloc(buf_px * sizeof(lv_color_t));
        g_buf2 = (lv_color_t *)malloc(buf_px * sizeof(lv_color_t));
        
        lv_display_set_buffers(g_disp, g_buf1, g_buf2,
                               buf_px * sizeof(lv_color_t),
                               LV_DISPLAY_RENDER_MODE_PARTIAL);

        lv_obj_invalidate(lv_screen_active());
        LOGI("impl_native_init: LVGL resized %dx%d", w, h);
    }
}

static void impl_draw_frame(JNIEnv *env, jobject obj){
    (void)env; (void)obj;

    // FIX: Use delta time for lv_tick_inc (prevents absolute time overflow bugs)
    static uint32_t last_time = 0;
    uint32_t current_time = now_ms();
    if (last_time == 0) last_time = current_time;
    uint32_t delta_time = current_time - last_time;
    lv_tick_inc(delta_time);
    last_time = current_time;

    drain_text_queue();        
    lv_timer_handler();        

    glBindTexture(GL_TEXTURE_2D, g_tex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, g_w, g_h,
                    GL_RGBA, GL_UNSIGNED_BYTE, g_fb);

    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(g_prog);
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);

    GLint ap = glGetAttribLocation(g_prog, "p");
    GLint au = glGetAttribLocation(g_prog, "u");
    glEnableVertexAttribArray(ap);
    glEnableVertexAttribArray(au);
    glVertexAttribPointer(ap, 2, GL_FLOAT, GL_FALSE, 16, (void *)0);
    glVertexAttribPointer(au, 2, GL_FLOAT, GL_FALSE, 16, (void *)8);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_tex);
    glUniform1i(glGetUniformLocation(g_prog, "t"), 0);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

static void impl_resize(JNIEnv *env, jobject obj, jint w, jint h){
    impl_native_init(env, obj, w, h);
}

static void impl_destroy(JNIEnv *env, jobject obj){
    (void)env; (void)obj;
    if(g_kbd_group){ lv_group_delete(g_kbd_group); g_kbd_group = NULL; }
    lv_deinit();
    free(g_buf1); free(g_buf2); free(g_fb);
    g_buf1 = g_buf2 = NULL; g_fb = NULL;
    glDeleteTextures(1, &g_tex);
    glDeleteBuffers(1,  &g_vbo);
    glDeleteProgram(g_prog);
    g_prog = 0;
    g_lv_inited = false; // Reset init flag
    LOGI("impl_destroy: cleaned up");
}

static void impl_push_touch(JNIEnv *env, jobject obj, jint action, jfloat x, jfloat y){
    (void)env; (void)obj;
    int pressed = (action == 0 || action == 2) ? 1 : 0; 
    push_touch((int32_t)x, (int32_t)y, pressed);
}

static void impl_push_key(JNIEnv *env, jobject obj, jint keyCode, jint action){
    (void)env; (void)obj;
    if(action == 0) return;    

    if(keyCode == 67){
        pthread_mutex_lock(&g_kmx);
        g_backspace_count++;
        pthread_mutex_unlock(&g_kmx);
        return;
    }

    uint32_t cp = 0;
    if(keyCode >= 29 && keyCode <= 54)       cp = (uint32_t)('a' + keyCode - 29);
    else if(keyCode >= 7  && keyCode <= 16)  cp = (uint32_t)('0' + keyCode - 7);
    else if(keyCode == 62)                   cp = ' ';

    if(cp) push_char(cp);
}

static void impl_push_text(JNIEnv *env, jobject obj, jint codepoint){
    (void)env; (void)obj;
    push_char((uint32_t)codepoint);
}

/* ═══════════════════════════════════════════════════════════════
   NativeBridge.registerNatives()
═══════════════════════════════════════════════════════════════ */
static JNINativeMethod g_renderer_methods[] = {
    { (char*)"nativeSurfaceCreated", (char*)"()V",   (void*)impl_surface_created },
    { (char*)"nativeInit",           (char*)"(II)V", (void*)impl_native_init  },
    { (char*)"nativeDrawFrame",      (char*)"()V",   (void*)impl_draw_frame   },
    { (char*)"nativeResize",         (char*)"(II)V", (void*)impl_resize       },
    { (char*)"nativeDestroy",        (char*)"()V",   (void*)impl_destroy      },
};

static JNINativeMethod g_view_methods[] = {
    { (char*)"nativePushTouch",   (char*)"(IFF)V", (void*)impl_push_touch   },
    { (char*)"nativePushKey",     (char*)"(II)V",  (void*)impl_push_key     },
    { (char*)"nativePushText",    (char*)"(I)V",   (void*)impl_push_text    },
};

extern "C" JNIEXPORT void JNICALL
Java_com_mylibrary_NativeBridge_registerNatives(
        JNIEnv *env, jclass /*bridgeClass*/,
        jobject rendererClass,
        jobject glViewClass)
{
    jclass cls_r = (jclass)rendererClass;
    jclass cls_v = (jclass)glViewClass;

    env->RegisterNatives(cls_r,
                         g_renderer_methods,
                         sizeof(g_renderer_methods)/sizeof(g_renderer_methods[0]));

    env->RegisterNatives(cls_v,
                         g_view_methods,
                         sizeof(g_view_methods)/sizeof(g_view_methods[0]));

    LOGI("registerNatives: Renderer + GLView methods registered");
}

extern "C" lv_group_t *lvgl_py_get_kbd_group(void){
    return g_kbd_group;
}