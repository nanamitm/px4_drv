/* SPDX-License-Identifier: MIT */
/* Native USB bootstrap hooks selected by the device registry. */
#ifndef MB86H_USB_BOOT_H
#define MB86H_USB_BOOT_H
#include "mb86H/usb/window.h"
#include "mb86H/native_download.h"

struct usb_boot_io {
    struct usb_native_ops native;
    /* Exact endpoint, bounded one-shot receive. No automatic resubmission. */
    int (*receive)(void *, uint8_t endpoint, void *, size_t, unsigned timeout_ms);
    int (*transmit)(void *, uint8_t endpoint, const void *, size_t, size_t *actual,
                    unsigned timeout_ms);
    int (*clear_halt)(void *, uint8_t endpoint);
    uint32_t (*now_ms)(void *);
    void (*delay_ms)(void *, unsigned);
};

enum usb_boot_stage {
    USB_BOOT_STAGE_NONE, USB_BOOT_STAGE_READ_STATE, USB_BOOT_STAGE_WAIT_READY,
    USB_BOOT_STAGE_READ_READY, USB_BOOT_STAGE_ACK_READY,
    USB_BOOT_STAGE_READ_PENDING, USB_BOOT_STAGE_ACK_PENDING,
    USB_BOOT_STAGE_BOOT0_READY, USB_BOOT_STAGE_FAILED,
    USB_BOOT_STAGE_CONFIGURE_DOWNLOAD, USB_BOOT_STAGE_ENABLE_COMPLETION,
    USB_BOOT_STAGE_EXECUTE, USB_BOOT_STAGE_WAIT_COMPLETE,
    USB_BOOT_STAGE_READ_COMPLETE, USB_BOOT_STAGE_ACK_COMPLETE,
    USB_BOOT_STAGE_RUNNING,
    USB_BOOT_STAGE_VERIFY_BOOT0, USB_BOOT_STAGE_CLEAR_ENDPOINT,
};

struct usb_boot_result {
    enum usb_boot_stage stage, failed_stage;
    uint16_t rawstate, ready_status, pending_status;
    unsigned control_count;
    int status;
    bool code_signature_verified;
};

struct usb_boot_download_ops {
    char magic[8];
    uint8_t endpoint;
    size_t chunk_size;
    unsigned timeout_ms;
    /* Additional version/role checks after common container validation. */
    int (*validate_image)(const uint8_t *, size_t);
    int (*prepare)(const struct usb_boot_io *, void *, struct usb_boot_result *);
    int (*finish)(const struct usb_boot_io *, void *, struct usb_boot_result *);
};

struct usb_boot_download_result {
    struct mb86H_native_download_result download;
    struct usb_boot_result boot;
};

struct usb_boot_ops {
    int (*observe)(const struct usb_boot_io *, void *, struct usb_boot_result *);
    int (*prepare)(const struct usb_boot_io *, void *, struct usb_boot_result *);
    int (*read)(const struct usb_boot_io *, void *, uint32_t, void *, size_t);
    /* Candidate acquisition range, not an established code/ROM identity. */
    uint32_t candidate_address;
    size_t candidate_size;
    const struct usb_boot_download_ops *download;
    const struct usb_native_window_profile *window;
};

int usb_boot_download(const struct usb_boot_download_ops *,
    const struct usb_boot_io *, void *, const uint8_t *, size_t,
    const struct mb86H_native_download_options *, struct usb_boot_download_result *);
int usb_boot_validate_image(const struct usb_boot_download_ops *,
                            const uint8_t *, size_t);
#endif
