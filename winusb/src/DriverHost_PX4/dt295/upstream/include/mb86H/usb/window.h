/* SPDX-License-Identifier: MIT */
#ifndef USB_WINDOW_H
#define USB_WINDOW_H

#ifdef __KERNEL__
#include <linux/types.h>
typedef u8 uint8_t;
typedef u16 uint16_t;
typedef u32 uint32_t;
#else
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#endif

#define USB_WINDOW_VENDOR_REQUEST 0xBCu
#define USB_WINDOW_VENDOR_VALUE 0x0800u
#define USB_WINDOW_ADDRESS_MIN 0x80000u
#define USB_WINDOW_ADDRESS_MAX 0xA0000u
#define USB_WINDOW_MAX_TRANSFER 0x200u
#define USB_WINDOW_CONTROL_CHUNK 64u

enum usb_window_direction { USB_WINDOW_OUT = 0, USB_WINDOW_IN = 1 };

struct usb_window_control {
    uint8_t request_type;
    uint8_t request;
    uint16_t value;
    uint16_t index;
    uint16_t length;
};

int usb_window_control_from_address(uint32_t address, size_t length,
                                    enum usb_window_direction direction,
                                    struct usb_window_control *out);
int usb_window_address_from_control(const struct usb_window_control *control,
                                    uint32_t *address);


/* Called with the physical owner's control lock held. Return zero only for
 * an exact transfer; preserve the backend's original negative status. */
struct usb_native_ops {
    int (*control)(void *, const struct usb_window_control *, void *, unsigned);
};
struct usb_native_window_profile {
    uint32_t read_window, write_window, staging;
    uint32_t ring_base, ring_bytes;
    bool dispatch_out;
    uint32_t memory_base, memory_bytes;
};
extern const struct usb_native_window_profile usb_torne_window_profile;
extern const struct usb_native_window_profile usb_dt295_window_profile;
int usb_native_window(const struct usb_native_ops *, void *,
    const struct usb_native_window_profile *, uint32_t, void *, size_t,
    bool writing, bool block_writes);
int usb_native_exchange(const struct usb_native_ops *, void *,
    const struct usb_native_window_profile *, const uint8_t[64], uint8_t[64], bool);
/* Native BC block reads proven independently in Sony and Selene drivers. */
int usb_native_read24(const struct usb_native_ops *, void *, uint32_t,
                      void *, size_t);
int usb_native_read16(const struct usb_native_ops *, void *, uint32_t, uint16_t *);
int usb_native_write16(const struct usb_native_ops *, void *, uint32_t, uint16_t);
int usb_native_mask16(const struct usb_native_ops *, void *, uint32_t,
                      unsigned shift, uint16_t mask, uint16_t value);
/* XZ boot0 property window and staging exchange, shared by physical owners. */
int usb_xz_window(const struct usb_native_ops *, void *, uint32_t,
                  void *, size_t, bool writing);
int usb_xz_exchange(const struct usb_native_ops *, void *,
                    const uint8_t request[64], uint8_t response[64]);
int usb_torne_window(const struct usb_native_ops *, void *, uint32_t,
                     void *, size_t, bool writing, bool block_writes);
int usb_torne_exchange(const struct usb_native_ops *, void *,
                       const uint8_t request[64], uint8_t response[64],
                       bool block_writes);

#endif
