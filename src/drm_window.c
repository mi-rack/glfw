//========================================================================
// GLFW 3.3 - www.glfw.org
//------------------------------------------------------------------------
// Copyright (c) 2016 Google Inc.
// Copyright (c) 2006-2016 Camilla Löwy <elmindreda@glfw.org>
//
// This software is provided 'as-is', without any express or implied
// warranty. In no event will the authors be held liable for any damages
// arising from the use of this software.
//
// Permission is granted to anyone to use this software for any purpose,
// including commercial applications, and to alter it and redistribute it
// freely, subject to the following restrictions:
//
// 1. The origin of this software must not be misrepresented; you must not
//    claim that you wrote the original software. If you use this software
//    in a product, an acknowledgment in the product documentation would
//    be appreciated but is not required.
//
// 2. Altered source versions must be plainly marked as such, and must not
//    be misrepresented as being the original software.
//
// 3. This notice may not be removed or altered from any source
//    distribution.
//
//========================================================================

#include "internal.h"

#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <sys/select.h>

#include <gbm.h>

#define GL_GLEXT_PROTOTYPES
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include <libinput.h>

static int createNativeWindow(_GLFWwindow* window,
                              const _GLFWwndconfig* wndconfig)
{
    // window->null.width = wndconfig->width;
    // window->null.height = wndconfig->height;

    return GLFW_TRUE;
}

GLFWbool DeviceOpen(char *card) {
  int fd = open(card, O_RDWR);// | O_CLOEXEC);
  if (fd < 0) {
    fprintf(stderr, "cannot open '%s': %m\n", card);
    return GLFW_FALSE;
  }

  _glfw.drm.fd = fd;

  return GLFW_TRUE;
}

  GLFWbool FindCrtc(int fd,
                drmModeRes* res,
                drmModeConnector* conn,
                uint32_t* crtc_out) {
    /* first try the currently conected encoder+crtc */
    if (conn->encoder_id) {
      drmModeEncoder* enc = drmModeGetEncoder(fd, conn->encoder_id);
      if (enc && enc->crtc_id) {
        uint32_t crtc = enc->crtc_id;
        // for (auto& dev : modeset_dev_list_) {
        //   if (dev->crtc == crtc) {
        //     break;
        //   }
        // }

        if (crtc >= 0) {
          drmModeFreeEncoder(enc);
          *crtc_out = crtc;
          return GLFW_TRUE;
        }
      }
      drmModeFreeEncoder(enc);
    }

    /* If the connector is not currently bound to an encoder or if the
     * encoder+crtc is already used by another connector (actually unlikely
     * but lets be safe), iterate all other available encoders to find a
     * matching CRTC. */
    for (int i = 0; i < conn->count_encoders; ++i) {
      drmModeEncoder* enc = drmModeGetEncoder(fd, conn->encoders[i]);
      if (!enc) {
        printf("cannot retrieve encoder %u:%u (%d): %m\n", i,
                conn->encoders[i], errno);
        continue;
      }

      /* iterate all global CRTCs */
      for (int j = 0; j < res->count_crtcs; ++j) {
        /* check whether this CRTC works with the encoder */
        if (!(enc->possible_crtcs & (1 << j)))
          continue;

        /* check that no other device already uses this CRTC */
        uint32_t crtc = res->crtcs[j];
        // for (auto& dev : modeset_dev_list_) {
        //   if (dev->crtc == crtc) {
        //     break;
        //   }
        // }

        /* we have found a CRTC, so save it and return */
        if (crtc >= 0) {
          drmModeFreeEncoder(enc);
          *crtc_out = crtc;
          return GLFW_TRUE;
        }
      }

      drmModeFreeEncoder(enc);
    }

    fprintf(stderr, "cannot find suitable CRTC for connector %u\n",
            conn->connector_id);
    return GLFW_FALSE;
  }

GLFWbool GetConnector() {
  /* retrieve resources */
  drmModeRes* res = drmModeGetResources(_glfw.drm.fd);
  if (!res) {
    fprintf(stderr, "cannot retrieve DRM resources (%d): %m\n", errno);
    return GLFW_FALSE;
  }

  /* iterate all connectors */
  for (int i = 0; i < res->count_connectors; ++i) {
    /* get information for each connector */
    drmModeConnector* conn = drmModeGetConnector(_glfw.drm.fd, res->connectors[i]);
    if (!conn) {
      fprintf(stderr, "cannot retrieve DRM connector %u:%u (%d): %m\n", i,
              res->connectors[i], errno);
      continue;
    }

    /* check if a monitor is connected */
    if (conn->connection != DRM_MODE_CONNECTED) {
      drmModeFreeConnector(conn);
      fprintf(stderr, "ignoring unused connector %u\n", conn->connector_id);
      continue;
    }

    /* check if there is at least one valid mode */
    if (!conn->count_modes) {
      drmModeFreeConnector(conn);
      fprintf(stderr, "no valid mode for connector %u\n", conn->connector_id);
      continue;
    }

    _glfw.drm.conn = conn->connector_id;
    _glfw.drm.mode = conn->modes[0];

    /* find a crtc for this connector */
    if (!FindCrtc(_glfw.drm.fd, res, conn, &_glfw.drm.crtc)) {
      fprintf(stderr, "cannot setup device for connector %u:%u (%d): %m\n", i,
              res->connectors[i], errno);
      drmModeFreeConnector(conn);
      continue;
    }

    /* free connector data and link device into global list */
    drmModeFreeConnector(conn);
    //modeset_dev_ = dev.get();
    //modeset_dev_list_.push_back(std::move(dev));

    // FIXME(dshwang): GetConnector() can support multiple connector, but
    // it makes page flip logic is so complicated. So use only one connector.
    // Most embedded devices have only one monitor.
    break;
  }

  /* free resources again */
  drmModeFreeResources(res);
  return GLFW_TRUE;
}

GLFWbool PageFlip(uint32_t fb_id, void* user_data) {
  int ret = drmModePageFlip(_glfw.drm.fd, _glfw.drm.crtc, fb_id, DRM_MODE_PAGE_FLIP_EVENT, user_data);
  if (ret) {
    fprintf(stderr, "failed to queue page flip: %m\n");
    return GLFW_FALSE;
  }
  return GLFW_TRUE;
}

void EGLSyncFence() {
  if (1) {//egl_.egl_sync_supported) {
    EGLSyncKHR sync =
        eglCreateSyncKHR(_glfw.egl.display, EGL_SYNC_FENCE_KHR, NULL);
    glFlush();
    eglClientWaitSyncKHR(_glfw.egl.display, sync, 0, EGL_FOREVER_KHR);
  } else {
    glFinish();
  }
}

void DidPageFlip(unsigned int sec, unsigned int usec) {
  _glfw.drm.page_flip_pending = 0;
  Framebuffer *back_fb = &_glfw.drm.framebuffers[_glfw.drm.front_buffer ^ 1];
  glBindFramebuffer(GL_FRAMEBUFFER, back_fb->gl_fb);
  // callback_(back_fb.gl_fb, sec * 1000000 + usec);
  // EGLSyncFence();
}

static void OnModesetPageFlipEvent(int fd,
                                   unsigned int frame,
                                   unsigned int sec,
                                   unsigned int usec,
                                   void* data) {
  DidPageFlip(sec, usec);
}

static void swapBuffersDRM(_GLFWwindow* window)
{
  eglSwapBuffers(_glfw.egl.display, window->context.egl.surface);

  fd_set fds;
  drmEventContext evctx = {};
  evctx.version = DRM_EVENT_CONTEXT_VERSION;
  evctx.page_flip_handler = OnModesetPageFlipEvent;

  Framebuffer *back_fb = &_glfw.drm.framebuffers[_glfw.drm.front_buffer ^ 1];
  if (!PageFlip(back_fb->fb_id, NULL))
    return;

  _glfw.drm.front_buffer ^= 1;

  _glfw.drm.page_flip_pending = 1;
  while (_glfw.drm.page_flip_pending) {
    FD_ZERO(&fds);
    FD_SET(0, &fds);
    FD_SET(_glfw.drm.fd, &fds);
    int ret = select(_glfw.drm.fd + 1, &fds, NULL, NULL, NULL);
    if (ret < 0) {
      fprintf(stderr, "%m\n");
      return;
    } else if (ret == 0) {
      fprintf(stderr, "select timeout!\n");
      return;
    }

    if (FD_ISSET(0, &fds)) {
      // Destroy();
      printf("!??\n");
    }
    if (FD_ISSET(_glfw.drm.fd, &fds)) {
      drmHandleEvent(_glfw.drm.fd, &evctx);
    }
  }
  if (FD_ISSET(0, &fds)) {
    printf("exit due to user-input\n");
  }
}

GLFWbool CreateFramebuffer(size_t width,
                         size_t height,
                         Framebuffer *framebuffer) {
  framebuffer->bo = gbm_bo_create(_glfw.drm.display, width, height, GBM_FORMAT_XRGB8888,
                                 GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
  if (!framebuffer->bo) {
    fprintf(stderr, "failed to create a gbm buffer.\n");
    return GLFW_FALSE;
  }

  framebuffer->fd = gbm_bo_get_fd(framebuffer->bo);
  if (framebuffer->fd < 0) {
    fprintf(stderr, "failed to get fb for bo: %d", framebuffer->fd);
    return GLFW_FALSE;
  }

  uint32_t handle = gbm_bo_get_handle(framebuffer->bo).u32;
  uint32_t stride = gbm_bo_get_stride(framebuffer->bo);
  uint32_t offset = 0;
  drmModeAddFB2(_glfw.drm.fd, width, height, GBM_FORMAT_XRGB8888, &handle,
                &stride, &offset, &framebuffer->fb_id, 0);
  if (!framebuffer->fb_id) {
    fprintf(stderr, "failed to create framebuffer from buffer object.\n");
    return GLFW_FALSE;
  }

  const EGLint khr_image_attrs[] = {EGL_DMA_BUF_PLANE0_FD_EXT,
                                    framebuffer->fd,
                                    EGL_WIDTH,
                                    width,
                                    EGL_HEIGHT,
                                    height,
                                    EGL_LINUX_DRM_FOURCC_EXT,
                                    GBM_FORMAT_XRGB8888,
                                    EGL_DMA_BUF_PLANE0_PITCH_EXT,
                                    stride,
                                    EGL_DMA_BUF_PLANE0_OFFSET_EXT,
                                    offset,
                                    EGL_NONE};

  framebuffer->image =
      eglCreateImageKHR(_glfw.egl.display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT,
                          NULL /* no client buffer */, khr_image_attrs);
  if (framebuffer->image == EGL_NO_IMAGE_KHR) {
    fprintf(stderr, "failed to make image from buffer object: %s\n",
            eglGetError());
    return GLFW_FALSE;
  }

  glGenTextures(1, &framebuffer->gl_tex);
  glBindTexture(GL_TEXTURE_2D, framebuffer->gl_tex);
  glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, framebuffer->image);
  glBindTexture(GL_TEXTURE_2D, 0);

  glGenFramebuffers(1, &framebuffer->gl_fb);
  glBindFramebuffer(GL_FRAMEBUFFER, framebuffer->gl_fb);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                         framebuffer->gl_tex, 0);

  if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
    fprintf(stderr,
            "failed framebuffer check for created target buffer: %x\n",
            glCheckFramebufferStatus(GL_FRAMEBUFFER));
    glDeleteFramebuffers(1, &framebuffer->gl_fb);
    glDeleteTextures(1, &framebuffer->gl_tex);
    return GLFW_FALSE;
  }

  return GLFW_TRUE;
}

GLFWbool ModeSetCrtc(uint32_t fb_id) {
  /* perform actual modesetting on each found connector+CRTC */
  _glfw.drm.saved_crtc = drmModeGetCrtc(_glfw.drm.fd, _glfw.drm.crtc);
  int ret = drmModeSetCrtc(_glfw.drm.fd, _glfw.drm.crtc, fb_id, 0, 0,
                           &_glfw.drm.conn, 1, &_glfw.drm.mode);
  if (ret) {
    fprintf(stderr, "cannot set CRTC for connector %u (%d): %m\n",
            _glfw.drm.conn, errno);
    return GLFW_FALSE;
  }
  return GLFW_TRUE;
}


//////////////////////////////////////////////////////////////////////////
//////                       GLFW platform API                      //////
//////////////////////////////////////////////////////////////////////////

int _glfwPlatformCreateWindow(_GLFWwindow* window,
                              const _GLFWwndconfig* wndconfig,
                              const _GLFWctxconfig* ctxconfig,
                              const _GLFWfbconfig* fbconfig)
{

  char *card = "/dev/dri/card0";
  fprintf(stdout, "using card: '%s'\n", card);
  if (!DeviceOpen(card))
    return GLFW_FALSE;

  if (!GetConnector())
    return GLFW_FALSE;

    dlopen("libglapi.so.0", RTLD_LAZY | RTLD_GLOBAL);
    _glfw.drm.display = gbm_create_device(_glfw.drm.fd);
    if (!_glfw.drm.display) {
      fprintf(stderr, "cannot create gbm device.\n");
      return GLFW_FALSE;
    }
    printf("device created\n");

    //EGLDisplay egl_display = weston_platform_get_egl_display(EGL_PLATFORM_GBM_KHR, _glfw.drm.display, NULL);

//printf("display created %p\n", egl_display);
/*    EGLint major, minor = 0;
    if (!eglInitialize(egl_display, &major, &minor)) {
      fprintf(stderr, "failed to initialize\n");
      return GLFW_FALSE;
    }

    printf("Using display %p with EGL version %d.%d\n", egl_.display, major,
           minor);

    printf("EGL Version \"%s\"\n", eglQueryString(egl_display, EGL_VERSION));
    printf("EGL Vendor \"%s\"\n", eglQueryString(egl_display, EGL_VENDOR));

    if (!eglBindAPI(EGL_OPENGL_ES_API)) {
      fprintf(stderr, "failed to bind api EGL_OPENGL_ES_API\n");
      return GLFW_FALSE;
    }*/

    printf("%d %d\n", _glfw.drm.mode.hdisplay, _glfw.drm.mode.vdisplay);

    if (ctxconfig->client != GLFW_NO_API)
    {
      if (!_glfwInitEGL())
          return GLFW_FALSE;
    }

  //     window->drm.window =
  //   gbm_surface_create (_glfw.drm.display,
  //                       _glfw.drm.mode.hdisplay,
  //                       _glfw.drm.mode.vdisplay,
  //                       GBM_BO_FORMAT_XRGB8888,
  //                       GBM_BO_USE_SCANOUT |
  //                       GBM_BO_USE_RENDERING);

  // if (!window->drm.window)
  //   {
  //     fprintf(stderr,"Failed to allocate surface\n");
  //     return GLFW_FALSE;
  //   }


    if (ctxconfig->client != GLFW_NO_API)
    {
      if (!_glfwCreateContextEGL(window, ctxconfig, fbconfig))
          return GLFW_FALSE;
    }

    window->context.makeCurrent(window);

    //DRMModesetter::Size display_size = drm_->GetDisplaySize();
    for (int i = 0; i < 2; i++)
    {
      if (!CreateFramebuffer(_glfw.drm.mode.hdisplay, _glfw.drm.mode.vdisplay, &_glfw.drm.framebuffers[i])) {
        fprintf(stderr, "cannot create framebuffer.\n");
        return GLFW_FALSE;
      }
    }

    _glfw.drm.front_buffer = 0;

    // Need to do the first mode setting before page flip.
    if (!ModeSetCrtc(_glfw.drm.framebuffers[_glfw.drm.front_buffer].fb_id))
      return GLFW_FALSE;

    window->context.swapBuffers = swapBuffersDRM;

  return GLFW_TRUE;

  /*
    if (!createNativeWindow(window, wndconfig))
        return GLFW_FALSE;

    if (ctxconfig->client != GLFW_NO_API)
    {
        if (ctxconfig->source == GLFW_NATIVE_CONTEXT_API ||
            ctxconfig->source == GLFW_OSMESA_CONTEXT_API)
        {
            if (!_glfwInitOSMesa())
                return GLFW_FALSE;
            if (!_glfwCreateContextOSMesa(window, ctxconfig, fbconfig))
                return GLFW_FALSE;
        }
        else
        {
            _glfwInputError(GLFW_API_UNAVAILABLE, "Null: EGL not available");
            return GLFW_FALSE;
        }
    }

    return GLFW_TRUE;*/
}

void _glfwPlatformDestroyWindow(_GLFWwindow* window)
{
    if (window->context.destroy)
        window->context.destroy(window);
}

void _glfwPlatformSetWindowTitle(_GLFWwindow* window, const char* title)
{
}

void _glfwPlatformSetWindowIcon(_GLFWwindow* window, int count,
                                const GLFWimage* images)
{
}

void _glfwPlatformSetWindowMonitor(_GLFWwindow* window,
                                   _GLFWmonitor* monitor,
                                   int xpos, int ypos,
                                   int width, int height,
                                   int refreshRate)
{
}

void _glfwPlatformGetWindowPos(_GLFWwindow* window, int* xpos, int* ypos)
{
}

void _glfwPlatformSetWindowPos(_GLFWwindow* window, int xpos, int ypos)
{
}

void _glfwPlatformGetWindowSize(_GLFWwindow* window, int* width, int* height)
{
    if (width)
        *width = 1920;//window->null.width;
    if (height)
        *height = 1080;//window->null.height;
}

void _glfwPlatformSetWindowSize(_GLFWwindow* window, int width, int height)
{
}

void _glfwPlatformSetWindowSizeLimits(_GLFWwindow* window,
                                      int minwidth, int minheight,
                                      int maxwidth, int maxheight)
{
}

void _glfwPlatformSetWindowAspectRatio(_GLFWwindow* window, int n, int d)
{
}

void _glfwPlatformGetFramebufferSize(_GLFWwindow* window, int* width, int* height)
{
    if (width)
        *width = 1920;//window->null.width;
    if (height)
        *height = 1080;//window->null.height;
}

void _glfwPlatformGetWindowFrameSize(_GLFWwindow* window,
                                     int* left, int* top,
                                     int* right, int* bottom)
{
}

void _glfwPlatformGetWindowContentScale(_GLFWwindow* window,
                                        float* xscale, float* yscale)
{
    if (xscale)
        *xscale = 1.f;
    if (yscale)
        *yscale = 1.f;
}

void _glfwPlatformIconifyWindow(_GLFWwindow* window)
{
}

void _glfwPlatformRestoreWindow(_GLFWwindow* window)
{
}

void _glfwPlatformMaximizeWindow(_GLFWwindow* window)
{
}

int _glfwPlatformWindowMaximized(_GLFWwindow* window)
{
    return GLFW_FALSE;
}

int _glfwPlatformWindowHovered(_GLFWwindow* window)
{
    return GLFW_FALSE;
}

int _glfwPlatformFramebufferTransparent(_GLFWwindow* window)
{
    return GLFW_FALSE;
}

void _glfwPlatformSetWindowResizable(_GLFWwindow* window, GLFWbool enabled)
{
}

void _glfwPlatformSetWindowDecorated(_GLFWwindow* window, GLFWbool enabled)
{
}

void _glfwPlatformSetWindowFloating(_GLFWwindow* window, GLFWbool enabled)
{
}

float _glfwPlatformGetWindowOpacity(_GLFWwindow* window)
{
    return 1.f;
}

void _glfwPlatformSetWindowOpacity(_GLFWwindow* window, float opacity)
{
}

void _glfwPlatformShowWindow(_GLFWwindow* window)
{
}


void _glfwPlatformRequestWindowAttention(_GLFWwindow* window)
{
}

void _glfwPlatformUnhideWindow(_GLFWwindow* window)
{
}

void _glfwPlatformHideWindow(_GLFWwindow* window)
{
}

void _glfwPlatformFocusWindow(_GLFWwindow* window)
{
}

int _glfwPlatformWindowFocused(_GLFWwindow* window)
{
    return GLFW_TRUE;
}

int _glfwPlatformWindowIconified(_GLFWwindow* window)
{
    return GLFW_FALSE;
}

int _glfwPlatformWindowVisible(_GLFWwindow* window)
{
    return GLFW_TRUE;
}

void _glfwPlatformPollEvents(void)
{

}

void _glfwPlatformWaitEvents(void)
{
  _glfwPlatformPollEvents();
}

void _glfwPlatformWaitEventsTimeout(double timeout)
{
  _glfwPlatformPollEvents();
}

void _glfwPlatformPostEmptyEvent(void)
{
}

void _glfwPlatformGetCursorPos(_GLFWwindow* window, double* xpos, double* ypos)
{
}

void _glfwPlatformSetCursorPos(_GLFWwindow* window, double x, double y)
{
}

void _glfwPlatformSetCursorMode(_GLFWwindow* window, int mode)
{
}

int _glfwPlatformCreateCursor(_GLFWcursor* cursor,
                              const GLFWimage* image,
                              int xhot, int yhot)
{
    return GLFW_TRUE;
}

int _glfwPlatformCreateStandardCursor(_GLFWcursor* cursor, int shape)
{
    return GLFW_TRUE;
}

void _glfwPlatformDestroyCursor(_GLFWcursor* cursor)
{
}

void _glfwPlatformSetCursor(_GLFWwindow* window, _GLFWcursor* cursor)
{
}

void _glfwPlatformSetClipboardString(const char* string)
{
}

const char* _glfwPlatformGetClipboardString(void)
{
    return NULL;
}

const char* _glfwPlatformGetScancodeName(int scancode)
{
    return "";
}

int _glfwPlatformGetKeyScancode(int key)
{
    return -1;
}

void _glfwPlatformGetRequiredInstanceExtensions(char** extensions)
{
}

int _glfwPlatformGetPhysicalDevicePresentationSupport(VkInstance instance,
                                                      VkPhysicalDevice device,
                                                      uint32_t queuefamily)
{
    return GLFW_FALSE;
}

VkResult _glfwPlatformCreateWindowSurface(VkInstance instance,
                                          _GLFWwindow* window,
                                          const VkAllocationCallbacks* allocator,
                                          VkSurfaceKHR* surface)
{
    // This seems like the most appropriate error to return here
    return VK_ERROR_INITIALIZATION_FAILED;
}

