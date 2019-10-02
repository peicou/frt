// video_kmsdrm.cpp
//scons platform=frt target=release frt_arch=pi4 tools=no module_webm_enabled=no
/*
 * FRT - A Godot platform targeting single board computers
 * Copyright (c) 2017-2019  Emanuele Fornara
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include "frt.h"

#include <stdio.h>
#include <string.h>
#include <sys/time.h>

#include "dl/gbm.gen.h"
#include "dl/drm.gen.h"

#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>

#include "bits/egl_base_context.h"

// on pi, skip EGL/GLESv2 in /opt/vc/lib
static const char *lib(const char *s) 
{
#if defined(__arm__) || defined(__aarch64__)
	static char buf[64]; // large enough
	strcpy(buf, "/opt/vc/lib/");
	strcat(buf, s);
	if (access(buf, R_OK) != 0)
		return s;
#if defined(__arm__)
	strcpy(buf, "/usr/lib/arm-linux-gnueabihf/");
#else
	strcpy(buf, "/usr/lib/aarch64-linux-gnu/");
#endif
	strcat(buf, s);
	return buf;
#else // !pi
	return s;
#endif
}

#define FRT_DL_SKIP
#if FRT_GLES_VERSION == 3
#include "dl/gles3.gen.h"
#else
#include "dl/gles2.gen.h"
#endif

static bool frt_load_gles(int version) {
#if FRT_GLES_VERSION == 3
	return frt_load_gles3(lib("libGLESv2.so.2"));
#else
	return frt_load_gles2(lib("libGLESv2.so.2"));
#endif
    return false;
}

namespace frt {

class EGLKmsdrmContext : public EGLBaseContext {
private:
    int _device;
    drmModeRes * _resources;
    drmModeConnector * _connector;
    uint32_t _connector_id;
    drmModeEncoder * _encoder;
    drmModeModeInfo  _mode_info;
    drmModeCrtc * _crtc;
    struct gbm_device * _gbm_device;
    struct gbm_surface * _gbm_surface;
    struct gbm_bo * _previous_bo = NULL;
    uint32_t _previous_fb;    
    struct gbm_bo * _bo;  
    uint32_t _handle;
    uint32_t _pitch;
    uint32_t _fb;
    uint64_t _modifier;
    EGLConfig _configs[32];
    int _config_index;

    drmModeConnector * _find_connector (drmModeRes *resources) 
    {
        //printf("_find connector \n");
        for (int i=0; i<resources->count_connectors; i++) 
        {
            drmModeConnector *connector = drmModeGetConnector (_device, resources->connectors[i]);
            if (connector->connection == DRM_MODE_CONNECTED) {return connector;}
            drmModeFreeConnector (connector);
        }

        return NULL; // if no connector found
    }

    drmModeEncoder * _find_encoder (drmModeRes *resources, drmModeConnector *connector) 
    {
        //printf("_find encoder \n");
        if (connector->encoder_id) {return drmModeGetEncoder (_device, connector->encoder_id);}
        return NULL; // if no encoder found
    }

    int _match_config_to_visual(EGLDisplay egl_display, EGLint visual_id, EGLConfig *configs, int count) 
    {
        //printf("match config to visual \n");
        EGLint id;
        for (int i = 0; i < count; ++i) 
        {
            if (!eglGetConfigAttrib(egl_display, configs[i], EGL_NATIVE_VISUAL_ID,&id)) continue;
            if (id == visual_id) 
            {
                //printf("   matched! %i \n",i);
                //printf("clearing eglGetError %i. 12288 is success fyi\n", eglGetError());
                return i;
            }
        }
        return -1;
    }

public:
    void init(int version)
    {
        //printf("gbmDrm init \n");
        EGLBoolean result;
        EGLint num_config;
        EGLint count=0;
#if FRT_GLES_VERSION == 2
        static EGLint attributes[] = {
                EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
                EGL_RED_SIZE, 8,
                EGL_GREEN_SIZE, 8,
                EGL_BLUE_SIZE, 8,
                EGL_ALPHA_SIZE, 0,
                EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
                EGL_NONE
                };

        static const EGLint context_attribs[] = {
                EGL_CONTEXT_CLIENT_VERSION, 2,
                EGL_NONE
                };
#else
        static EGLint attributes[] = {
                EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
                EGL_RED_SIZE, 8,
                EGL_GREEN_SIZE, 8,
                EGL_BLUE_SIZE, 8,
                EGL_ALPHA_SIZE, 0,
                EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR,
                EGL_NONE
                };

        static const EGLint context_attribs[] = {
                EGL_CONTEXT_CLIENT_VERSION, 3,
                EGL_NONE
                };
#endif
        _device = open ("/dev/dri/card1", O_RDWR);
        if (_device < 0)
        {
            printf("failed to open card\n");
            return;
        } 
        else
        {
            _resources = drmModeGetResources (_device);
            if (_resources == nullptr)
            {
                printf("failed to get resources\n");
                return;
            }
            _connector = _find_connector (_resources);
            if (_connector == nullptr)
            {
                printf("failed to get connector. no fb?\n");
                return;
            }
        }
        _connector_id = _connector->connector_id;
        _mode_info = _connector->modes[0];
        _encoder = _find_encoder (_resources, _connector);
        _crtc = drmModeGetCrtc (_device, _encoder->crtc_id);
        drmModeFreeEncoder (_encoder);
        drmModeFreeConnector (_connector);
        drmModeFreeResources (_resources);
        _gbm_device = gbm_create_device (_device);
        _gbm_surface = gbm_surface_create (_gbm_device, _mode_info.hdisplay, _mode_info.vdisplay, GBM_FORMAT_XRGB8888, GBM_BO_USE_SCANOUT|GBM_BO_USE_RENDERING);
        display = eglGetDisplay (_gbm_device);
    
        result = eglInitialize (display, NULL ,NULL);
        if (result == EGL_FALSE)
            printf("eglInitialize failed.\n");
        result = eglBindAPI (EGL_OPENGL_ES_API);
        if (result == EGL_FALSE)
            printf("eglBindAPI failed.\n");
        
        eglGetConfigs(display, NULL, 0, &count);
        //printf("EGL received %i configs\n", count);
        result = eglChooseConfig (display, attributes, &_configs[0], count, &num_config);
        if (result == EGL_FALSE) 
            printf("eglChooseConfig failed.\n");

        _config_index = _match_config_to_visual(display,GBM_FORMAT_XRGB8888,&_configs[0],num_config);
        context = eglCreateContext (display, _configs[_config_index], EGL_NO_CONTEXT, context_attribs);
        if (context == EGL_NO_CONTEXT)
            printf("eglCreateContext failed: %i.\n", eglGetError());
    }

    void create_surface() {
        surface = eglCreateWindowSurface (display, _configs[_config_index], _gbm_surface, NULL);
        if (surface == EGL_NO_SURFACE)
            printf("video_kmsdrm: eglCreateWindowSurface failed., %i\n", eglGetError());
    }

    void swap_buffers()
    {
        eglSwapBuffers (display, surface);
        _bo = gbm_surface_lock_front_buffer (_gbm_surface);
        _handle = gbm_bo_get_handle (_bo).u32;
        _pitch = gbm_bo_get_stride (_bo);
        drmModeAddFB (_device, _mode_info.hdisplay, _mode_info.vdisplay, 24, 32, _pitch, _handle, &_fb);
        drmModeSetCrtc (_device, _crtc->crtc_id, _fb, 0, 0, &_connector_id, 1, &_mode_info);
        if (_previous_bo) 
        {
            drmModeRmFB (_device, _previous_fb);
            gbm_surface_release_buffer (_gbm_surface, _previous_bo);
        }
        _previous_bo = _bo;
        _previous_fb = _fb;
    }
    //! getters
    int getDevice() { return _device; }
    drmModeCrtc * getCrtc() { return _crtc; }
    uint32_t * getConnector_id() { return &_connector_id; }
    struct gbm_bo * getPrevious_bo() {return _previous_bo; } 
    uint32_t getPrevious_fb() { return _previous_fb; }
    struct gbm_surface * getGbm_surface() { return _gbm_surface; }
    struct gbm_device * getGbm_device() { return _gbm_device; }
};

class VideoKmsdrm : public Video, public ContextGL {
private:
    EGLKmsdrmContext _egl;
    bool _initialized;
    Vec2 _screen_size;
    bool _vsync;
    drmModeCrtc * _crtc;
    int _EGLVersion = 0;

    void init_egl(Vec2 size) {
        //printf("kmsdrm init_egl\n");
        _egl.init(_EGLVersion);
        _egl.create_surface();
        _egl.make_current();

        _crtc = _egl.getCrtc();

        _screen_size.x = get_window_width();
        _screen_size.y = get_window_height();

        _initialized = true;
    }
    void cleanup_egl() 
    {
        if (!_initialized)
            return;

        if (_crtc == nullptr)
        return;

        drmModeSetCrtc (_egl.getDevice(), _crtc->crtc_id, _crtc->buffer_id, _crtc->x, _crtc->y, _egl.getConnector_id(), 1, &_crtc->mode);
        drmModeFreeCrtc (_crtc);
        if (_egl.getPrevious_bo()) 
        {
            drmModeRmFB (_egl.getDevice(), _egl.getPrevious_fb());
            gbm_surface_release_buffer (_egl.getGbm_surface(), _egl.getPrevious_bo());
        }
        _egl.destroy_surface();
        gbm_surface_destroy (_egl.getGbm_surface());
        _egl.cleanup();
        gbm_device_destroy (_egl.getGbm_device());
        close (_egl.getDevice());
        _initialized = false;
    }

public:
    // Module
    VideoKmsdrm()
        : _initialized(false), _vsync(true) {}
    const char *get_id() const { return "video_kmsdrm"; }
    bool probe() {
        //printf("   inside bgmDrm probe\n");
        if (!frt_load_gbm("libgbm.so.1"))
        {
            printf("failed to load libgbm\n");
            return false;
        }
        if (!frt_load_drm("libdrm.so.2"))
        {
            printf("failed to load libdrm\n");
            return false;
        }
        if (!frt_load_egl(lib("libEGL.so.1")))
        {
            printf("failed to load EGL\n");
            return false;
		}
        return true;
    }
    void cleanup() {
        cleanup_egl();
    }
    // Video
    Vec2 get_screen_size() const { return _screen_size; }

    Vec2 get_view_size() const { return _screen_size; }

	ContextGL *create_the_gl_context(int version, Vec2 size) {
		if (!frt_load_gles(version))
			return 0;
		_EGLVersion = version;
		_screen_size = size;
		return this;
	}        

    bool provides_quit() { return false; }

    void set_title(const char *title) {}
	
    Vec2 move_pointer(const Vec2 &screen) {
		return Vec2(0,0);
	}
	
    void show_pointer(bool enable) {}
	
    int get_window_height() 
    { 
        int h = 0;
        if (_crtc != nullptr)
        {
            h = (int)_crtc->height;  
        }     
        return h;
    }

    int get_window_width() 
    { 
        int w = 0;
        if (_crtc != nullptr)
        {
            w = (int)_crtc->width;  
        }     
        return w;
    }

    // ContextGL
    void release_current() {
        _egl.release_current();
    }
    void make_current() {
        _egl.make_current();
    }
    void swap_buffers() {
        _egl.swap_buffers();
    }
    bool initialize() {
        init_egl(_screen_size);
        return true;
    }
    void set_use_vsync(bool use) {
        _egl.swap_interval(use ? 1 : 0);
        _vsync = use;
    }
    bool is_using_vsync() const { return _vsync; }
};

FRT_REGISTER(VideoKmsdrm)

} // namespace frt
