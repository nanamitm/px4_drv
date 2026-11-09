/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef DTP_TORNE_CLOCK_H
#define DTP_TORNE_CLOCK_H
#include <stdint.h>
#define DTP_TORNE_FRAME_REGISTER 0x00fdbc1cu
struct dtp_torne_clock { uint32_t us, frame; uint8_t initialized; };
/* 11bitの1msカウンターを延長する。呼出し間隔は2.048秒未満が必要。 */
uint32_t dtp_torne_clock_update(struct dtp_torne_clock *,uint32_t raw);
#endif
