#include <vulkan/vulkan_core.h>
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <vulkan/vulkan.h>

#include "capture.h"

struct crtc {
  uint32_t id;

  uint64_t seq;
  uint64_t ns;

  uint64_t delta_seq;
  uint64_t delta_ns;
};

static struct crtc crtcs[64] = {0};
static size_t crtcs_len = 0;

static int monitor_crtc(int fd, struct crtc *crtc) {
  uint32_t queue_flags =
      DRM_CRTC_SEQUENCE_RELATIVE | DRM_CRTC_SEQUENCE_NEXT_ON_MISS;
  int ret =
      drmCrtcQueueSequence(fd, crtc->id, queue_flags, 1, NULL, (uint64_t)crtc);
  if (ret != 0 && errno != EINVAL) {
    perror("drmCrtcQueueSequence");
    return ret;
  }
  return 0;
}

int *dma_buf_fd;
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

static void handle_sequence(int fd, uint64_t seq, uint64_t ns, uint64_t data) {
  struct crtc *crtc = (struct crtc *)data;
  assert(seq > crtc->seq);
  assert(ns > crtc->ns);
  crtc->delta_seq = seq - crtc->seq;
  crtc->delta_ns = ns - crtc->ns;
  crtc->seq = seq;
  crtc->ns = ns;

  monitor_crtc(fd, crtc);
  
  //prepareImage(fd);
  drmModeCrtcPtr crtc_my = drmModeGetCrtc(fd, crtc->id);
  if (crtc_my) {
    drmModeFB2Ptr fb = drmModeGetFB2(fd, crtc_my->buffer_id);
    drmModeFreeCrtc(crtc_my);
    
    if (fb) {
      int nplanes = 0;
      initDmaBufFDs(fd, fb, dma_buf_fd, &nplanes);
      
      capture_init_shtex(fb->width, fb->height, DRM_FORMAT_XRGB8888, fb->pitches,
                         fb->offsets, fb->modifier, 0, false, VK_COLORSPACE_SRGB_NONLINEAR_KHR, nplanes, dma_buf_fd);
      cleanupDmaBufFDs(fb, dma_buf_fd, &nplanes);
      // capture_update_socket();
    }
  }
}

static const char usage[] =
    "Usage: drm_monitor [options...]\n"
    "\n"
    "  -d              Specify DRM device (default /dev/dri/card0).\n"
	  "  -o              Specify output connector (by name).\n"
    "  -h              Show help message and quit.\n";

int main(int argc, char *argv[]) {
  char *device_path = "/dev/dri/card0";
	char *use_output_name = NULL;
  int opt;
  while ((opt = getopt(argc, argv, "hd:o:")) != -1) {
    switch (opt) {
    case 'h':
      printf("%s", usage);
      return EXIT_SUCCESS;
    case 'd':
      device_path = optarg;
      break;
		case 'o':
			use_output_name = optarg;
			break;
    default:
      return EXIT_FAILURE;
    }
  }
  int fd = open(device_path, O_RDONLY);
  if (fd < 0) {
    perror("open");
    return 1;
  }
  drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);

  drmModeRes *res = drmModeGetResources(fd);
  if (res == NULL) {
    perror("drmModeGetResources");
    return 1;
  }
  assert((size_t)res->count_crtcs < sizeof(crtcs) / sizeof(crtcs[0]));


	char output_name[64] = {0};
	size_t conn_len = (size_t)res->count_connectors;
	for (int i = 0; i < conn_len; i++) {
		int conn_id = res->connectors[i];
		drmModeConnector *conn = drmModeGetConnector(fd, conn_id); 
		if (!conn) {
			continue;
		}
		const char *conn_type_name = drmModeGetConnectorTypeName(conn->connector_type);
		snprintf(output_name, sizeof(output_name), "%s-%d", conn_type_name, conn->connector_type_id);
		if (conn->connection == DRM_MODE_CONNECTED && (!use_output_name || !strcmp(output_name, use_output_name))) {
			uint32_t encoder_id = conn->encoder_id;
			drmModeFreeConnector(conn);
			drmModeEncoder *encoder = drmModeGetEncoder(fd, encoder_id);
			if (!encoder) {
				continue;
			}

			uint32_t crtc_id = encoder->crtc_id;
			drmModeFreeEncoder(encoder);
			//We're only using one, wasteful, convert to single instance
			crtcs[0].id = crtc_id;
			int ret = drmCrtcGetSequence(fd, crtcs[0].id, &crtcs[0].seq, &crtcs[0].ns);
			if (ret != 0 && errno != EINVAL) {
				continue;
			}

			if (monitor_crtc(fd, &crtcs[0]) == 0) {
				break;
			}

		}
	}

  drmModeFreeResources(res);

  capture_init();
  capture_update_socket();
  
  dma_buf_fd = (int *)malloc(sizeof(int) * 4);

  while (1) {
    drmEventContext ctx = {
        .version = 4,
        .sequence_handler = handle_sequence,
    };
    if (drmHandleEvent(fd, &ctx) != 0) {
      perror("drmHandleEvent");
      return 1;
    }
  }

  capture_stop();
  close(fd);
  return 0;
}
