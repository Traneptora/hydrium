#ifndef HYD_ENCODER_H_
#define HYD_ENCODER_H_

#include <stddef.h>

#include "libhydrium/libhydrium.h"

HYDStatusCode hyd_send_tile_pre(HYDEncoder *encoder, uint32_t tile_x, uint32_t tile_y, int is_last);
HYDStatusCode hyd_encode_xyb_buffer(HYDEncoder *encoder, size_t tile_x, size_t tile_y);

#endif /* HYD_ENCODER_H_ */
