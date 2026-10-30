#ifndef CRD_CAPTURE_H
#define CRD_CAPTURE_H

#include <stdint.h>

struct crd_stream_info {
	uint32_t node_id;
	int32_t x, y, width, height;
	char mapping_id[128];
};

int capture_init(void);
int capture_stream_info(struct crd_stream_info *out);
int capture_open_pipewire_remote(void);

#endif
