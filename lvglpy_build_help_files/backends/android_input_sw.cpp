/*
 * backends/android_input_sw.cpp
 *
 * Android Software Render backend for lvglpy.
 * Compiled when: cmake -B build -DLV_BACKEND=ANDROID_SW
 *
 * Synced with the dual-thread Java architecture:
 *
 *   Main thread:  LvglPyGLView.onTouchEvent / onKeyDown / onKeyUp
 *                   → nativePushTouch / nativePushKey
 *                   → push into mutex-protected C++ queues
 *
 *   GL thread:    LvglPyRenderer.onSurfaceCreated/Changed/DrawFrame
 *                   → nativeInit / nativeDrawFrame / nativeDestroy
 *                   → drains queues + drives LVGL
 *
 * .so portability:
 *   NativeBridge.registerNatives(rendererClass, glViewClass) is called
 *   from MainActivity — Class objects passed from Java so C++ never
 *   needs a hardcoded package string. Fully portable .so.
 *
 * implements backend_base.h:
 *   lvgl_py_display_init()   — no-op on Android (surface owns dimensions)
 *   lvgl_py_backend_name()   — returns "Android-SW"
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
   Display / GL state  (GL thread only after init)
═══════════════════════════════════════════════════════════════ */
static int           g_w    = 0, g_h = 0;
static uint8_t      *g_fb   = NULL;
static lv_color_t   *g_buf1 = NULL;
static lv_color_t   *g_buf2 = NULL;
static lv_display_t *g_disp = NULL;

static GLuint g_tex  = 0;
static GLuint g_prog = 0;
static GLuint g_vbo  = 0;

/* ═══════════════════════════════════════════════════════════════
   Touch queue  — main thread writes, GL thread reads
   nativePushTouch(action, x, y) mirrors Android MotionEvent actions:
     ACTION_DOWN = 0, ACTION_UP = 1, ACTION_MOVE = 2
═══════════════════════════════════════════════════════════════ */
#define TOUCH_Q 64
typedef struct { int32_t x, y, pressed; } TouchEvent;

static TouchEvent      g_tq[TOUCH_Q];
static int             g_tqh = 0, g_tqt = 0;
static pthread_mutex_t g_tmx = PTHREAD_MUTEX_INITIALIZER;
static int32_t         g_tx  = 0, g_ty = 0, g_tp = 0;

/* ═══════════════════════════════════════════════════════════════
   Text / key queue  — main thread writes, GL thread reads
   nativePushKey(keyCode, action):
     Handles printable chars → push_char (UTF-8 encode)
     Handles KEYCODE_DEL    → backspace counter
   Text string path:
     nativePushText(codepoint) for InputConnection.commitText()
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
   LVGL flush — blit dirty rect into g_fb (BGRA → RGBA)
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
   LVGL indev callbacks  (GL thread — called by lv_timer_handler)
═══════════════════════════════════════════════════════════════ */
static void touch_cb(lv_indev_t *indev, lv_indev_data_t *data){
    pthread_mutex_lock(&g_tmx);
    while(g_tqh != g_tqt){
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
   drain_text_queue  (GL thread — called before lv_timer_handler)
   Injects ALL buffered text at once — no per-frame char limit.
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
   Push helpers  (main thread — called from JNI)
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
    /*
     * No-op on Android — GLSurfaceView owns the surface.
     * Real LVGL init happens in impl_native_init() when
     * onSurfaceChanged fires and dimensions are known.
     */
    (void)width; (void)height; (void)resizable;
    LOGI("lvgl_py_display_init: Android-SW — waiting for surface");
}

const char *lvgl_py_backend_name(void){
    return "Android-SW";
}

/* ═══════════════════════════════════════════════════════════════
   JNI implementations  (registered via NativeBridge.registerNatives)
   GL thread: impl_native_init, impl_draw_frame, impl_destroy
   Main thread: impl_push_touch, impl_push_key, impl_push_text
═══════════════════════════════════════════════════════════════ */

/*
 * Called by LvglPyRenderer.onSurfaceChanged()
 * Also called by LvglPyRenderer.nativeResize() on rotation.
 * GL thread.
 */
static void impl_native_init(JNIEnv *env, jobject obj, jint w, jint h){
    (void)env; (void)obj;

    /* first call — set up GL objects */
    if(g_prog == 0){
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
        LOGI("impl_native_init: GL objects created");
    }

    g_w = w; g_h = h;
    glViewport(0, 0, w, h);

    /* (re)allocate pixel buffer */
    free(g_fb);
    g_fb = (uint8_t *)calloc((size_t)(w * h * 4), 1);

    glBindTexture(GL_TEXTURE_2D, g_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, g_fb);

    /* init / reinit LVGL */
    lv_init();

    size_t buf_px = (size_t)(w * (h / 10));
    free(g_buf1); free(g_buf2);
    g_buf1 = (lv_color_t *)malloc(buf_px * sizeof(lv_color_t));
    g_buf2 = (lv_color_t *)malloc(buf_px * sizeof(lv_color_t));

    g_disp = lv_display_create(w, h);
    lv_display_set_flush_cb(g_disp, flush_cb);
    lv_display_set_buffers(g_disp, g_buf1, g_buf2,
                           buf_px * sizeof(lv_color_t),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);

    /* touch indev */
    lv_indev_t *touch = lv_indev_create();
    lv_indev_set_type(touch, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(touch, touch_cb);

    /* keyboard indev — keeps focus group alive */
    g_kbd_indev = lv_indev_create();
    lv_indev_set_type(g_kbd_indev, LV_INDEV_TYPE_KEYPAD);
    lv_indev_set_read_cb(g_kbd_indev, keyboard_cb);

    g_kbd_group = lv_group_create();
    lv_indev_set_group(g_kbd_indev, g_kbd_group);

    LOGI("impl_native_init: LVGL ready %dx%d", w, h);
}

/*
 * Called by LvglPyRenderer.nativeDrawFrame()
 * Python on_frame() runs BEFORE this in LvglPyRenderer.onDrawFrame.
 * GL thread.
 */
static void impl_draw_frame(JNIEnv *env, jobject obj){
    (void)env; (void)obj;

    lv_tick_inc(now_ms());
    drain_text_queue();        /* inject text before LVGL ticks */
    lv_timer_handler();        /* LVGL: drains touch_cb, renders, flush_cb */

    /* upload SW framebuffer → GL texture */
    glBindTexture(GL_TEXTURE_2D, g_tex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, g_w, g_h,
                    GL_RGBA, GL_UNSIGNED_BYTE, g_fb);

    /* draw fullscreen quad */
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

/*
 * Called by LvglPyRenderer.nativeResize()
 * Resize is just a re-init with new dimensions.
 * GL thread.
 */
static void impl_resize(JNIEnv *env, jobject obj, jint w, jint h){
    impl_native_init(env, obj, w, h);
}

/*
 * Called by LvglPyRenderer.nativeDestroy()
 * GL thread.
 */
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
    LOGI("impl_destroy: cleaned up");
}

/*
 * Called by LvglPyGLView.onTouchEvent()
 * action mirrors Android MotionEvent:
 *   0 = ACTION_DOWN, 1 = ACTION_UP, 2 = ACTION_MOVE
 * Main thread — pushes into mutex queue only.
 */
static void impl_push_touch(JNIEnv *env, jobject obj,
                             jint action, jfloat x, jfloat y){
    (void)env; (void)obj;
    int pressed = (action == 0 || action == 2) ? 1 : 0;  /* DOWN/MOVE=1, UP=0 */
    push_touch((int32_t)x, (int32_t)y, pressed);
}

/*
 * Called by LvglPyGLView.onKeyDown / onKeyUp
 * Handles Android KeyEvent keyCodes.
 * Main thread — pushes into mutex queue only.
 *
 * KEYCODE_DEL (67) → backspace
 * Printable chars  → push_char (best-effort via keyCode mapping)
 *
 * NOTE: For full Unicode text input use nativePushText (commitText path).
 *       nativePushKey handles hardware key events only.
 */
static void impl_push_key(JNIEnv *env, jobject obj,
                           jint keyCode, jint action){
    (void)env; (void)obj;
    if(action == 0) return;    /* only process key-down events */

    /* KEYCODE_DEL = 67 */
    if(keyCode == 67){
        pthread_mutex_lock(&g_kmx);
        g_backspace_count++;
        pthread_mutex_unlock(&g_kmx);
        return;
    }

    /*
     * Basic printable ASCII via keyCode.
     * KEYCODE_A=29 .. KEYCODE_Z=54  → 'a'-'z'
     * KEYCODE_0=7  .. KEYCODE_9=16  → '0'-'9'
     * KEYCODE_SPACE=62              → ' '
     * For full text input prefer the commitText / nativePushText path.
     */
    uint32_t cp = 0;
    if(keyCode >= 29 && keyCode <= 54)       cp = (uint32_t)('a' + keyCode - 29);
    else if(keyCode >= 7  && keyCode <= 16)  cp = (uint32_t)('0' + keyCode - 7);
    else if(keyCode == 62)                   cp = ' ';

    if(cp) push_char(cp);
}

/*
 * Called by LvglPyGLView InputConnection.commitText() path.
 * One codepoint per call — handles full Unicode including emoji.
 * Main thread.
 */
static void impl_push_text(JNIEnv *env, jobject obj, jint codepoint){
    (void)env; (void)obj;
    push_char((uint32_t)codepoint);
}

/* ═══════════════════════════════════════════════════════════════
   NativeBridge.registerNatives()
   Called from MainActivity — Class objects passed from Java.
   C++ never calls FindClass with a hardcoded package string.
   Fully portable .so — works in any app package.
═══════════════════════════════════════════════════════════════ */
static JNINativeMethod g_renderer_methods[] = {
    { (char*)"nativeInit",        (char*)"(II)V",  (void*)impl_native_init  },
    { (char*)"nativeDrawFrame",   (char*)"()V",    (void*)impl_draw_frame   },
    { (char*)"nativeResize",      (char*)"(II)V",  (void*)impl_resize       },
    { (char*)"nativeDestroy",     (char*)"()V",    (void*)impl_destroy      },
};

static JNINativeMethod g_view_methods[] = {
    { (char*)"nativePushTouch",   (char*)"(IFF)V", (void*)impl_push_touch   },
    { (char*)"nativePushKey",     (char*)"(II)V",  (void*)impl_push_key     },
    { (char*)"nativePushText",    (char*)"(I)V",   (void*)impl_push_text    },
};

/*
 * Called from NativeBridge.registerNatives(rendererClass, glViewClass).
 * Java passes Class objects — no package string needed in C++.
 */
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

/*
 * lvgl_py_get_kbd_group()
 * Exposed via nanobind so Python can add textarea widgets to
 * the keyboard focus group:
 *   grp = lvglpy.get_kbd_group()
 *   lvglpy.group_add_obj(grp, my_textarea)
 */
extern "C" lv_group_t *lvgl_py_get_kbd_group(void){
    return g_kbd_group;
}