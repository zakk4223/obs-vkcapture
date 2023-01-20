#include "capture.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#define MSG(fmt, ...) printf(fmt "\n", ##__VA_ARGS__)

#define ASSERT(cond)                                                           \
  if (!(cond)) {                                                               \
    MSG("ERROR @ %s:%d: (%s) failed", __FILE__, __LINE__, #cond);              \
    return 0;                                                                  \
  }

uint32_t lastGoodPlane = 0;

int handle_id = 0;
drmModeFB2Ptr prepareImage(int drmfd) {

  drmModePlaneResPtr planes = drmModeGetPlaneResources(drmfd);

  // Check the first plane (or last good)
  drmModePlanePtr plane = drmModeGetPlane(drmfd, planes->planes[lastGoodPlane]);
  uint32_t fb_id = plane->fb_id;
  drmModeFreePlane(plane);

  // Find a good plane
  if (fb_id == 0) {
    for (uint32_t i = 0; i < planes->count_planes; ++i) {
      drmModePlanePtr plane = drmModeGetPlane(drmfd, planes->planes[i]);

      if (plane->fb_id != 0) {
        drmModeFB2Ptr fb = drmModeGetFB2(drmfd, plane->fb_id);
        if (fb == NULL) {
          // ctx->lastGoodPlane = 0;
          continue;
        }
        if (fb->handles[handle_id]) {
          if (fb->width == 256 && fb->height == 256)
            continue;
        }
        // drmModeFreeFB2(fb);

        lastGoodPlane = i;
        fb_id = plane->fb_id;
        // MSG("%d, %#x", i, fb_id);

        drmModeFreePlane(plane);
        return fb;
        break;
      } else {
        drmModeFreePlane(plane);
      }
    }
  } else {
    return drmModeGetFB2(drmfd, fb_id);
  }

  drmModeFreePlaneResources(planes);

  // MSG("%#x", fb_id);
  return NULL;
}

void initDmaBufFDs(int drmfd, drmModeFB2Ptr fb, int *dma_buf_fd, int *nplanes) {
  for (int i = 0; i < 4; i++) {
    if (fb->handles[i] == 0) {
      *nplanes = i;
      break;
    }
    drmPrimeHandleToFD(drmfd, fb->handles[i], O_RDONLY, (dma_buf_fd + i));
  }
}

void cleanupDmaBufFDs(drmModeFB2Ptr fb, int *dma_buf_fd, int *nplanes) {
  for (int i = 0; i < *nplanes; i++)
    if (dma_buf_fd[i] >= 0)
      close(dma_buf_fd[i]);
  if (fb)
    drmModeFreeFB2(fb);
}

int main(int argc, char *argv[]) {
  const char *card = "/dev/dri/card0";
  int drmfd = open(card, O_RDONLY);
  if (drmfd < 0) {
    perror("Cannot open card");
    return 1;
  }
  drmSetClientCap(drmfd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);

  const int available = drmAvailable();
  if (!available)
    return 0;

  // Find DRM video source
  int *dma_buf_fd = (int *)malloc(sizeof(int) * 4);

  drmModeFB2Ptr fb = prepareImage(drmfd);

  int nplanes = 0;
  initDmaBufFDs(drmfd, fb, dma_buf_fd, &nplanes);

  MSG("Number of planes: %d", nplanes);
  if (nplanes == 0) {
    MSG("Not permitted to get fb handles. Run either with sudo, or setcap "
        "cap_sys_admin+ep %s",
        argv[0]);
    cleanupDmaBufFDs(fb, dma_buf_fd, &nplanes);
    close(drmfd);
    return 0;
  }

  /*EGLImage eimg = create_dmabuf_egl_image(fb->width, fb->height,
                                      DRM_FORMAT_XRGB8888, nplanes, dma_buf_fd,
                                      fb->pitches, fb->offsets, fb->modifier);
  ASSERT(eimg);*/
  capture_init();
  capture_update_socket();
  
  while (true) {
    // Find DRM video source
    cleanupDmaBufFDs(fb, dma_buf_fd, &nplanes);

    fb = prepareImage(drmfd);
    initDmaBufFDs(drmfd, fb, dma_buf_fd, &nplanes);
    
    capture_init_shtex(fb->width, fb->height, DRM_FORMAT_XRGB8888, fb->pitches,
                       fb->offsets, fb->modifier, 0, false, nplanes, dma_buf_fd);
    // capture_update_socket();
    usleep(1000000/60);
  }

  capture_stop();
}
