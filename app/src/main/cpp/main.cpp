/*
 * Copyright (C) 2010 The Android Open Source Project
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
 *
 */

#include <EGL/egl.h>
#include <GLES/gl.h>
#include <android/log.h>
#include <android/sensor.h>
#include <android_native_app_glue.h>
#include <jni.h>

#include <cassert>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <dlfcn.h>

#define LOGI(...) \
  ((void)__android_log_print(ANDROID_LOG_INFO, "native-activity", __VA_ARGS__))
#define LOGW(...) \
  ((void)__android_log_print(ANDROID_LOG_WARN, "native-activity", __VA_ARGS__))

class Engine {
private:
    typedef struct SavedState_ {
        float angle;
        int32_t x;
        int32_t y;
    } SavedState;

    struct Context {
        struct android_app *app;
        ASensorManager *sensorManager;
        const ASensor *accelerometerSensor;
        ASensorEventQueue *sensorEventQueue;
        int animating;
        EGLDisplay display;
        EGLSurface surface;
        EGLContext context;
        int32_t width;
        int32_t height;
        SavedState state;
    } ctx;

public:
    inline bool isAnimating() const { return ctx.animating; }

    void init(struct android_app *state) {
        memset(&ctx, 0, sizeof(ctx));
        ctx.app = state;
        ctx.sensorManager = acquireASensorManagerInstance();
        ctx.accelerometerSensor = ASensorManager_getDefaultSensor(ctx.sensorManager,
                                                                  ASENSOR_TYPE_ACCELEROMETER);
        ctx.sensorEventQueue = ASensorManager_createEventQueue(ctx.sensorManager, state->looper,
                                                               LOOPER_ID_USER, nullptr, nullptr);
        if (state->savedState != nullptr) {
            // We are starting with a previous saved state; restore from it.
            ctx.state = *(SavedState *) state->savedState;
        }
    }

    /**
     * Initialize an EGL context for the current display.
     */
    int initDisplay() {
        /*
         * Here specify the attributes of the desired configuration.
         * Below, we select an EGLConfig with at least 8 bits per color
         * component compatible with on-screen windows
         */
        const EGLint attr[] = {EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
                               EGL_BLUE_SIZE, 8,
                               EGL_GREEN_SIZE, 8,
                               EGL_RED_SIZE, 8,
                               EGL_NONE};
        EGLint w, h, format;
        EGLint numConfigs;
        EGLConfig config = nullptr;
        EGLSurface surface;
        EGLContext context;

        EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);

        eglInitialize(display, nullptr, nullptr);

        /* Here, the application chooses the configuration it desires.
         * find the best match if possible, otherwise use the very first one
         */
        eglChooseConfig(display, attr, nullptr, 0, &numConfigs);
        std::unique_ptr<EGLConfig[]> supportedConfigs(new EGLConfig[numConfigs]);
        assert(supportedConfigs);
        eglChooseConfig(display, attr, supportedConfigs.get(), numConfigs,
                        &numConfigs);
        assert(numConfigs);
        auto i = 0;
        for (; i < numConfigs; i++) {
            auto &cfg = supportedConfigs[i];
            EGLint r, g, b, d;
            if (eglGetConfigAttrib(display, cfg, EGL_RED_SIZE, &r) &&
                eglGetConfigAttrib(display, cfg, EGL_GREEN_SIZE, &g) &&
                eglGetConfigAttrib(display, cfg, EGL_BLUE_SIZE, &b) &&
                eglGetConfigAttrib(display, cfg, EGL_DEPTH_SIZE, &d) && r == 8 &&
                g == 8 && b == 8 && d == 0) {
                config = supportedConfigs[i];
                break;
            }
        }
        if (i == numConfigs) {
            config = supportedConfigs[0];
        }

        if (config == nullptr) {
            LOGW("Unable to initialize EGLConfig");
            return -1;
        }

        /* EGL_NATIVE_VISUAL_ID is an attribute of the EGLConfig that is
         * guaranteed to be accepted by ANativeWindow_setBuffersGeometry().
         * As soon as we picked a EGLConfig, we can safely reconfigure the
         * ANativeWindow buffers to match, using EGL_NATIVE_VISUAL_ID. */
        eglGetConfigAttrib(display, config, EGL_NATIVE_VISUAL_ID, &format);
        surface = eglCreateWindowSurface(display, config, ctx.app->window, nullptr);
        context = eglCreateContext(display, config, nullptr, nullptr);

        if (eglMakeCurrent(display, surface, surface, context) == EGL_FALSE) {
            LOGW("Unable to eglMakeCurrent");
            return -1;
        }

        eglQuerySurface(display, surface, EGL_WIDTH, &w);
        eglQuerySurface(display, surface, EGL_HEIGHT, &h);

        ctx.display = display;
        ctx.context = context;
        ctx.surface = surface;
        ctx.width = w;
        ctx.height = h;
        ctx.state.angle = 0;

        // Check openGL on the system
        auto opengl_info = {GL_VENDOR, GL_RENDERER, GL_VERSION, GL_EXTENSIONS};
        for (auto name: opengl_info) {
            auto info = glGetString(name);
            LOGI("OpenGL Info: %s", info);
        }

        // Initialize GL state.
        glHint(GL_PERSPECTIVE_CORRECTION_HINT, GL_FASTEST);
        glEnable(GL_CULL_FACE);
        glShadeModel(GL_SMOOTH);
        glDisable(GL_DEPTH_TEST);
        return 0;
    }

    /**
     * Tear down the EGL context currently associated with the display.
     */
    void termDisplay() {
        if (ctx.display != EGL_NO_DISPLAY) {
            eglMakeCurrent(ctx.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            if (ctx.context != EGL_NO_CONTEXT) {
                eglDestroyContext(ctx.display, ctx.context);
            }
            if (ctx.surface != EGL_NO_SURFACE) {
                eglDestroySurface(ctx.display, ctx.surface);
            }
            eglTerminate(ctx.display);
        }
        ctx.animating = false;
        ctx.display = EGL_NO_DISPLAY;
        ctx.context = EGL_NO_CONTEXT;
        ctx.surface = EGL_NO_SURFACE;
    }

    /**
     * Just the current frame in the display.
     */
    void drawFrame() const {
        if (ctx.display == nullptr) {
            // No display.
            return;
        }
        // Just fill the screen with a color.
        glClearColor(((float) ctx.state.x) / (float) ctx.width, ctx.state.angle,
                     ((float) ctx.state.y) / (float) ctx.height, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        eglSwapBuffers(ctx.display, ctx.surface);
    }

    int32_t onInputEvent(AInputEvent *event) {
        if (AInputEvent_getType(event) == AINPUT_EVENT_TYPE_MOTION) {
            ctx.animating = true;
            ctx.state.x = (int) AMotionEvent_getX(event, 0);
            ctx.state.y = (int) AMotionEvent_getY(event, 0);
            return 1;
        }
        return 0;
    }

    /**
     * The system has asked us to save our current state.  Do so.
     */
    void onSaveState() const {
        ctx.app->savedState = malloc(sizeof(SavedState));
        *((SavedState *) ctx.app->savedState) = ctx.state;
        ctx.app->savedStateSize = sizeof(SavedState);
    }

    /**
     * The window is being shown, get it ready.
     */
    void onInitWindow() {
        if (ctx.app->window != nullptr) {
            this->initDisplay();
            this->drawFrame();
        }
    }

    /**
     * The window is being hidden or closed, clean it up.
     */
    void onTermWindow() {
        this->termDisplay();
    }

    /**
     * System focus event
     */
    void onFocus(bool gained) {
        if (gained) {
            // When our app gains focus, we start monitoring the accelerometer.
            if (ctx.accelerometerSensor != nullptr) {
                ASensorEventQueue_enableSensor(ctx.sensorEventQueue,
                                               ctx.accelerometerSensor);
                // We'd like to get 60 events per second (in us).
                ASensorEventQueue_setEventRate(ctx.sensorEventQueue,
                                               ctx.accelerometerSensor,
                                               (1000L / 60) * 1000);
            }
        } else {
            // When our app loses focus, we stop monitoring the accelerometer.
            // This is to avoid consuming battery while not being used.
            if (ctx.accelerometerSensor != nullptr) {
                ASensorEventQueue_disableSensor(ctx.sensorEventQueue,
                                                ctx.accelerometerSensor);
            }
            // Also stop animating.
            ctx.animating = false;
            this->drawFrame();
        }
    }

    void processSensorEvents() const {
        if (ctx.accelerometerSensor != nullptr) {
            ASensorEvent event;
            while (ASensorEventQueue_getEvents(ctx.sensorEventQueue, &event, 1) > 0) {
                LOGI("accelerometer: x=%f y=%f z=%f",
                     event.acceleration.x,
                     event.acceleration.y,
                     event.acceleration.z);
            }
        }
    }

    void animate() {
        if (ctx.animating) {
            // Done with events; draw next animation frame.
            ctx.state.angle += .01f;
            if (ctx.state.angle > 1) {
                ctx.state.angle = 0;
            }
            // Drawing is throttled to the screen update rate, so there
            // is no need to do timing here.
            drawFrame();
        }
    }

private:
    /**
     * Workaround ASensorManager_getInstance() deprecation false alarm
     * for Android-N and before, when compiling with NDK-r15
     * @return a sensor manager instance
     */
    ASensorManager *acquireASensorManagerInstance() const {
        if (!ctx.app) return nullptr;
        typedef ASensorManager *(*PF_GETINSTANCEFORPACKAGE)(const char *name);
        void *androidHandle = dlopen("libandroid.so", RTLD_NOW);
        auto getInstanceForPackageFunc = (PF_GETINSTANCEFORPACKAGE) dlsym(
                androidHandle, "ASensorManager_getInstanceForPackage");
        if (getInstanceForPackageFunc) {
            JNIEnv *env = nullptr;
            ctx.app->activity->vm->AttachCurrentThread(&env, nullptr);
            jclass android_content_Context = env->GetObjectClass(ctx.app->activity->clazz);
            jmethodID midGetPackageName = env->GetMethodID(
                    android_content_Context, "getPackageName", "()Ljava/lang/String;");
            auto packageName =
                    (jstring) env->CallObjectMethod(ctx.app->activity->clazz, midGetPackageName);
            const char *nativePackageName =
                    env->GetStringUTFChars(packageName, nullptr);
            ASensorManager *mgr = getInstanceForPackageFunc(nativePackageName);
            env->ReleaseStringUTFChars(packageName, nativePackageName);
            ctx.app->activity->vm->DetachCurrentThread();
            if (mgr) {
                dlclose(androidHandle);
                return mgr;
            }
        }
        typedef ASensorManager *(*PF_GETINSTANCE)();
        auto getInstanceFunc =
                (PF_GETINSTANCE) dlsym(androidHandle, "ASensorManager_getInstance");
        // by all means at this point, ASensorManager_getInstance should be available
        assert(getInstanceFunc);
        dlclose(androidHandle);
        return getInstanceFunc();
    }
};

/**
 * This is the main entry point of a native application that is using
 * android_native_app_glue.  It runs in its own thread, with its own
 * event loop for receiving input events and doing other things.
 */
__attribute__((unused)) void android_main(struct android_app *state) {
    Engine engine{};
    state->userData = &engine;
    state->onAppCmd = [](struct android_app *app, int32_t cmd) {
        auto *engine = (Engine *) app->userData;
        switch (cmd) {
            case APP_CMD_SAVE_STATE:
                engine->onSaveState();
                break;
            case APP_CMD_INIT_WINDOW:
                engine->onInitWindow();
                break;
            case APP_CMD_TERM_WINDOW:
                engine->onTermWindow();
                break;
            case APP_CMD_GAINED_FOCUS:
                engine->onFocus(true);
                break;
            case APP_CMD_LOST_FOCUS:
                engine->onFocus(false);
                break;
            default:
                break;
        }
    };
    state->onInputEvent = [](struct android_app *app, AInputEvent *event) -> int32_t {
        return ((Engine *) app->userData)->onInputEvent(event);
    };
    engine.init(state);

    // loop waiting for stuff to do.
    while (true) {
        // Read all pending events.
        int ident;
        int events;
        struct android_poll_source *source;

        // If not animating, we will block forever waiting for events.
        // If animating, we loop until all events are read, then continue
        // to draw the next frame of animation.
        while ((ident = ALooper_pollAll(engine.isAnimating() ? 0 : -1, nullptr, &events,
                                        (void **) &source)) >= 0) {
            // Process this event.
            if (source != nullptr) {
                source->process(state, source);
            }

            // If a sensor has data, process it now.
            if (ident == LOOPER_ID_USER) {
                engine.processSensorEvents();
            }

            // Check if we are exiting.
            if (state->destroyRequested != 0) {
                engine.termDisplay();
                return;
            }
        }

        engine.animate();
    }
}
