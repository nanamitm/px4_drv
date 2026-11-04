#include "mb86H/usb/window.h"
#include "mb86H/protocol/constants.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

struct native_mock {
    uint8_t mapped[4];
    uint8_t saw_ring_map[4];
    unsigned controls;
    unsigned block_writes;
};

static int native_control(void *opaque, const struct usb_window_control *control,
                          void *data, unsigned timeout)
{
    struct native_mock *mock = opaque;

    assert(control->request == USB_WINDOW_VENDOR_REQUEST);
    assert(timeout == 1000);
    mock->controls++;
    if (control->length == 4 && control->request_type == 0x40) {
        memcpy(mock->mapped, data, sizeof(mock->mapped));
        if (!memcmp(data, "\x82\0\0\0", 4))
            memcpy(mock->saw_ring_map, data, sizeof(mock->saw_ring_map));
    } else if (control->length == 4 && control->request_type == 0xc0) {
        memcpy(data, mock->mapped, sizeof(mock->mapped));
    } else if (control->request_type == 0xc0) {
        memset(data, 0xa5, control->length);
    } else if (control->request_type == 0x40 && control->length == 64) {
        mock->block_writes++;
    }
    return 0;
}

int main(void)
{
    struct usb_window_control c;
    uint32_t address = 0;
    assert(usb_window_control_from_address(0x8219Cu, 0x110u,
                                           USB_WINDOW_OUT, &c) == 0);
    assert(c.request_type == 0x40u && c.request == 0xBCu);
    assert(c.value == 0x0800u && c.index == 0x219Cu && c.length == 0x110u);
    assert(usb_window_address_from_control(&c, &address) == 0);
    assert(address == 0x8219Cu);
    assert(usb_window_control_from_address(0x9FFF0u, 0x20u,
                                           USB_WINDOW_IN, &c) != 0);
    assert(usb_window_control_from_address(0x80000u, 0x200u,
                                           USB_WINDOW_OUT, &c) == 0);
    {
        struct native_mock mock = {0};
        struct usb_native_ops ops = { .control = native_control };
        uint8_t ring[64];

        assert(usb_torne_window(&ops, &mock, MB86H_TORNE_TS_RING_BASE,
                                ring, sizeof(ring), false, false) == 0);
        assert(!memcmp(mock.saw_ring_map, "\x82\0\0\0", 4));
        for (unsigned i = 0; i < sizeof(ring); ++i)
            assert(ring[i] == 0xa5);
        assert(usb_torne_window(&ops, &mock,
                                MB86H_TORNE_TS_RING_BASE + MB86H_TORNE_TS_RING_BYTES - 4,
                                ring, 8, false, false) == -22);
    }
    {
        struct native_mock mock = {.mapped = {0, 0xf0, 0, 0}};
        struct usb_native_ops ops = {.control = native_control};
        uint8_t ram[64];
        assert(!usb_native_window(&ops, &mock, &usb_dt295_window_profile,
            MB86H_DT295_AUX_RAM_ADDRESS, ram, sizeof(ram), false, false));
        for (unsigned i = 0; i < sizeof(ram); ++i) assert(ram[i] == 0xa5);
        assert(!memcmp(mock.mapped, "\0\xf0\0\0", 4));
        assert(!usb_native_window(&ops, &mock, &usb_dt295_window_profile,
            MB86H_DT295_AUX_RAM_ADDRESS, ram, sizeof(ram), true, true));
        assert(mock.block_writes == 1);
        unsigned before = mock.controls;
        assert(usb_native_window(&ops, &mock, &usb_dt295_window_profile,
            MB86H_DT295_AUX_RAM_ADDRESS + MB86H_DT295_AUX_RAM_BYTES - 4,
            ram, 8, false, false) == -22);
        assert(usb_native_window(&ops, &mock, &usb_dt295_window_profile,
            MB86H_DT295_AUX_RAM_ADDRESS - 4, ram, 4, false, false) == -22);
        assert(mock.controls == before);
    }
    return 0;
}
