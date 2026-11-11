/* SPDX-License-Identifier: MIT */
#ifndef USB_LIBUSB_H
#define USB_LIBUSB_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include "mb86H/transport.h"
#include "mb86H/usb/boot.h"

struct usb_device;
struct usb_stream;

enum usb_result {
    USB_OK = 0,
    USB_NOT_FOUND = -2200,
    USB_AMBIGUOUS = -2201,
    USB_CLOSED = -2202,
    USB_SUBMIT = -2203,
    USB_TRANSFER = -2204,
    USB_ARGUMENT = -2205,
    USB_OPEN = -2206,
};

struct usb_control_step {
    uint8_t request_type;
    uint8_t request;
    uint16_t value;
    uint16_t index;
    uint8_t *data;
    uint16_t length;
    uint16_t actual_length;
    /* Native libusb submission rc and transfer-status enum; -1 means no
     * completion was received. Retained even when result is USB_TRANSFER. */
    int submit_result;
    int transfer_status;
    unsigned timeout_ms;
};

struct usb_stream_stats {
    size_t completed_bytes;
    size_t completed_transfers;
    size_t failed_transfers;
    int last_error;
};

struct usb_libusb_identity {
    uint16_t vendor, product, bcd_device;
    uint8_t bus, address;
};
struct usb_libusb_receive_status {
    int submitted, submit_result, transfer_status, actual_length;
    int descriptor_result;
    unsigned requested_length, timeout_ms;
    uint8_t endpoint, transfer_type, bytes[64];
};

/* Every one-shot native endpoint completion, including failures. */
struct usb_libusb_native_step {
    /* 0: endpoint transfer; 1: explicit clear-halt (no transfer completion). */
    unsigned operation;
    uint8_t endpoint, transfer_type;
    unsigned requested_length, timeout_ms;
    int descriptor_result, submitted, submit_result, transfer_status, actual_length;
    int status;
};
typedef void (*usb_native_observer)(void *, const struct usb_libusb_native_step *,
                                    const uint8_t *data);
int usb_libusb_observe_native(struct usb_device *, usb_native_observer, void *);
/* External vendor image; distinct from the resident XZFB bundle. Cooperative
 * cancellation at operation boundaries; in-flight transfers drain first.
 * A failed prepare/transfer/activate fences this handle from another download.
 * result contains native errno, partial progress and device completion state. */
int usb_libusb_download_native(struct usb_device *, const uint8_t *, size_t,
    const struct mb86H_native_download_options *, struct usb_boot_download_result *);

/* Observation runs under the owner's control lock; it must not reenter USB.
 * Raw bytes are borrowed and available even on a partial/failed completion. */
typedef void (*usb_control_observer)(void *, const struct usb_control_step *,
                                     const uint8_t *raw_data);
int usb_libusb_get_identity(struct usb_device *, struct usb_libusb_identity *);
int usb_libusb_get_receive_status(struct usb_device *, struct usb_libusb_receive_status *);
int usb_libusb_observe_control(struct usb_device *, usb_control_observer, void *);
/* Device-selected native power-on preparation, before firmware upload. */
int usb_libusb_prepare_boot0(struct usb_device *, struct usb_boot_result *);
int usb_libusb_observe_boot0(struct usb_device *, struct usb_boot_result *);

/* Return non-zero to make the stream terminal after this callback. The
 * callback owner retains stream/buffer ownership and still performs the
 * normal cancel-and-drain close path. */
typedef int (*usb_stream_callback)(void *opaque, int result,
                                      const uint8_t *data, size_t size);

/* Borrowed transport wrapper, valid until device close. */
const struct mb86H_transport *usb_libusb_transport(struct usb_device *device);

int usb_libusb_count_devices(unsigned *count);
int usb_libusb_open(struct usb_device **out);
/* Exact supported USB identity only; no VID/PID wildcard matching. */
int usb_libusb_count_target(uint16_t vendor, uint16_t product, unsigned *count);
int usb_libusb_open_target(struct usb_device **out, uint16_t vendor,
                           uint16_t product);
/* Loader enables Sony 64-byte OUT staging only after it has installed and
 * verified the version-bound boot0 receiver patch. */
int usb_libusb_set_block_writes(struct usb_device *device, bool enabled);
int usb_libusb_close(struct usb_device *device);
int usb_libusb_reset(struct usb_device *device);
int usb_libusb_control(struct usb_device *device,
                       struct usb_control_step *steps, size_t step_count,
                       unsigned timeout_ms);
/* Loader-only property space, distinct from the narrower vendor window. */
int usb_libusb_boot0_read(struct usb_device *, uint32_t, void *, size_t);
int usb_libusb_boot0_write(struct usb_device *, uint32_t, const void *, size_t);
int usb_libusb_window_write(struct usb_device *device, uint32_t address,
                            const uint8_t *data, size_t size,
                            unsigned timeout_ms);
int usb_libusb_window_read(struct usb_device *device, uint32_t address,
                           uint8_t *data, size_t size,
                           unsigned timeout_ms);
int usb_libusb_stream_start(struct usb_device *device, uint8_t endpoint,
                         uint8_t *buffer, size_t buffer_size,
                         usb_stream_callback callback, void *opaque,
                         struct usb_stream **out);
int usb_libusb_stream_cancel_and_drain(struct usb_stream *stream);
struct usb_stream_stats usb_libusb_stream_stats(struct usb_stream *stream);

/*
 * callback runs on the sole event worker. It must not call the blocking
 * control, cancel-and-drain or close functions. Input buffers stay owned by
 * the caller until cancel-and-drain or close returns. close drains every
 * active transfer before freeing it. close must be externally serialized
 * against every other API call on its device. actual_length records short IN
 * reads; short OUT writes fail and are never retried. Successful bulk
 * callbacks re-submit their same stream automatically until cancellation or
 * a non-zero callback return.
 * Callback data is valid only until the callback returns and must be consumed
 * or copied there. A cancel/close error leaves ownership active; retry it
 * before releasing its buffer or opaque state. The caller serializes a
 * cancel-and-drain against a new start on that same endpoint.
 */

#endif
