// Intel Wireless Series receiver. Protocol reference: intel-wings 0.2.
// USB descriptors verified on hardware: interface 0 alt 1, interrupt
// IN 0x81 (27 bytes), OUT 0x01 (25 bytes). EP 0x02 is control, not data.
#include "intel_wireless_series.h"

#if CFG_TUH_ENABLED
#include <stdio.h>
#include <string.h>
#include "core/buttons.h"
#include "core/input_event.h"
#include "core/router/router.h"
#include "core/services/players/manager.h"

enum { INTEL_WIRELESS_SERIES_SLOTS = 8, INTEL_WIRELESS_SERIES_RX_SIZE = 27, INTEL_WIRELESS_SERIES_TX_SIZE = 6 };

typedef struct {
    bool gamepad;
    bool submitted;
} intel_wireless_series_slot_t;

typedef struct {
    bool opened, configured, rx_busy, tx_busy;
    uint8_t pending_activation, tx_slot;
    tusb_desc_endpoint_t ep_in, ep_out;
    intel_wireless_series_slot_t slots[INTEL_WIRELESS_SERIES_SLOTS];
    CFG_TUSB_MEM_ALIGN uint8_t rx[INTEL_WIRELESS_SERIES_RX_SIZE];
    CFG_TUSB_MEM_ALIGN uint8_t tx[INTEL_WIRELESS_SERIES_TX_SIZE];
} intel_wireless_series_receiver_t;

static CFG_TUSB_MEM_SECTION intel_wireless_series_receiver_t receivers[CFG_TUH_DEVICE_MAX + 1];

static intel_wireless_series_receiver_t *receiver(uint8_t addr)
{
    return addr && addr <= CFG_TUH_DEVICE_MAX ? &receivers[addr] : NULL;
}

static void forget_slot(uint8_t addr, intel_wireless_series_receiver_t *r, uint8_t slot)
{
    if (r->slots[slot].submitted) {
        router_device_disconnected(addr, slot);
        remove_players_by_address(addr, slot);
    }
    memset(&r->slots[slot], 0, sizeof(r->slots[slot]));
    r->pending_activation &= ~(1u << slot);
}

static void submit_pad(uint8_t addr, intel_wireless_series_receiver_t *r, uint8_t slot,
                       const uint8_t *packet)
{
    input_event_t event = {
        .dev_addr = addr, .instance = slot,
        .type = INPUT_TYPE_GAMEPAD, .transport = INPUT_TRANSPORT_USB,
        .layout = LAYOUT_SEGA_6BUTTON, .button_count = 8,
        .analog = {128, 128, 128, 128, 0, 0},
    };
    // Match Joypad's M30 six-face-button convention; keep all eight
    // physical action buttons distinct: Z/C -> L1/R1, shoulders -> L2/R2.
    static const uint32_t buttons[] = {
        JP_BUTTON_B1, JP_BUTTON_B2, JP_BUTTON_R1, JP_BUTTON_B3,
        JP_BUTTON_B4, JP_BUTTON_L1, JP_BUTTON_L2, JP_BUTTON_R2,
    };
    if (packet) {
        for (unsigned i = 0; i < 8; i++) {
            if (packet[11] & (1u << i)) event.buttons |= buttons[i];
        }
        if (packet[12] & 1) event.buttons |= JP_BUTTON_S2; // Start
        if (packet[12] & 2) event.buttons |= JP_BUTTON_A1; // Shift
        if (packet[12] & 4) event.buttons |= JP_BUTTON_S1; // Mouse
        if (packet[9] == 0x81) event.buttons |= JP_BUTTON_DL;
        if (packet[9] == 0x7f) event.buttons |= JP_BUTTON_DR;
        if (packet[10] == 0x81) event.buttons |= JP_BUTTON_DU;
        if (packet[10] == 0x7f) event.buttons |= JP_BUTTON_DD;
    }
    router_submit_input(&event);
    r->slots[slot].submitted = true;
}

static void process_packet(uint8_t addr, intel_wireless_series_receiver_t *r,
                           const uint8_t *p, uint32_t len)
{
    if (!len || len > INTEL_WIRELESS_SERIES_RX_SIZE) return;
    switch (p[0] & 7) {
    case 1: { // Input; bytes through 12 are required.
        if (len < 13) return;
        uint8_t slot = p[4] & 7;
        if (r->slots[slot].gamepad) submit_pad(addr, r, slot, p);
        break;
    }
    case 3: { // Information / activation / ready.
        if (len < 3) return;
        uint8_t slot = p[1] & 7;
        uint8_t op = p[2];
        if (op == 1 || op == 0x0a || op == 0x0b || op == 0x0c) {
            if (len < 4) return;
            bool gamepad = p[3] == 2;
            // A fresh activation is a new session, even if type is unchanged.
            if (op == 1 || r->slots[slot].gamepad != gamepad) {
                forget_slot(addr, r, slot);
            }
            r->slots[slot].gamepad = gamepad;
            if (op == 1 && gamepad) r->pending_activation |= 1u << slot;
        } else if (op == 4) {
            r->slots[slot].gamepad = true;
        } else if (op == 5) { // Mouse ready: this slot is no longer a pad.
            forget_slot(addr, r, slot);
        }
        break;
    }
    case 6: // Zero-state message: release held inputs, retain known type.
        if (len >= 2) {
            uint8_t slot = p[1] & 7;
            if (r->slots[slot].submitted) submit_pad(addr, r, slot, NULL);
        }
        break;
    default:
        break;
    }
}

static bool transfer(uint8_t addr, uint8_t ep, uint8_t *buf, uint16_t len)
{
    if (!usbh_edpt_claim(addr, ep)) return false;
    if (usbh_edpt_xfer(addr, ep, buf, len)) return true;
    usbh_edpt_release(addr, ep);
    return false;
}

void intel_wireless_series_task(void)
{
    for (unsigned addr = 1; addr <= CFG_TUH_DEVICE_MAX; addr++) {
        intel_wireless_series_receiver_t *r = &receivers[addr];
        if (!r->configured) continue;
        if (!r->rx_busy) {
            r->rx_busy = transfer(addr, r->ep_in.bEndpointAddress, r->rx, sizeof(r->rx));
        }
        if (!r->tx_busy && r->pending_activation) {
            uint8_t slot = 0;
            while (!(r->pending_activation & (1u << slot))) slot++;
            const uint8_t reply[INTEL_WIRELESS_SERIES_TX_SIZE] = {slot, 1, 0xff, 0, 1, 0x63};
            memcpy(r->tx, reply, sizeof(reply));
            if (transfer(addr, r->ep_out.bEndpointAddress, r->tx, sizeof(r->tx))) {
                r->tx_slot = slot;
                r->tx_busy = true;
                r->pending_activation &= ~(1u << slot);
            }
        }
    }
}

static bool intel_wireless_series_init(void)
{
    memset(receivers, 0, sizeof(receivers));
    return true;
}

void intel_wireless_series_disconnect(uint8_t addr)
{
    intel_wireless_series_receiver_t *r = receiver(addr);
    if (!r || !r->opened) return;
    // Clear every routed output BEFORE deleting any player mappings. In
    // SHIFT mode deleting one slot changes the indices of the remaining
    // players, so interleaving router cleanup and deletion misses outputs.
    for (uint8_t slot = 0; slot < INTEL_WIRELESS_SERIES_SLOTS; slot++) {
        if (r->slots[slot].submitted) router_device_disconnected(addr, slot);
    }
    remove_players_by_address(addr, -1);
    memset(r, 0, sizeof(*r));
}

static bool intel_wireless_series_deinit(void)
{
    for (unsigned addr = 1; addr <= CFG_TUH_DEVICE_MAX; addr++) intel_wireless_series_disconnect(addr);
    return true;
}

static bool intel_wireless_series_open(uint8_t rhport, uint8_t addr,
                       const tusb_desc_interface_t *itf, uint16_t max_len)
{
    (void)rhport;
    uint16_t vid, pid;
    intel_wireless_series_receiver_t *r = receiver(addr);
    bool got = r && tuh_vid_pid_get(addr, &vid, &pid);
    if (!r || !got || vid != 0x8086 || pid != 0xc013) return false;
    if (max_len < sizeof(*itf)) return false;
    // Own the boot mouse too, avoiding instance collisions with RF slots.
    // Keyboard/mouse RF devices are intentionally outside this driver's scope.
    if (itf->bInterfaceNumber == 1) return true;
    if (itf->bInterfaceNumber != 0 || r->opened) return false;

    // TinyUSB supplies this interface's descriptors INCLUDING alternates.
    // Never open the boot endpoint: its packet size differs from alt 1.
    bool selected = false, found_in = false, found_out = false;
    tusb_desc_endpoint_t ep_in = {0}, ep_out = {0};
    const uint8_t *p = (const uint8_t *)itf;
    for (uint16_t pos = 0; pos < max_len;) {
        if (max_len - pos < 2 || p[pos] < 2 || p[pos] > max_len - pos) return false;
        if (p[pos + 1] == TUSB_DESC_INTERFACE) {
            if (p[pos] < sizeof(*itf)) return false;
            const tusb_desc_interface_t *alt = (const tusb_desc_interface_t *)(p + pos);
            selected = alt->bInterfaceNumber == 0 && alt->bAlternateSetting == 1;
        } else if (selected && p[pos + 1] == TUSB_DESC_ENDPOINT) {
            if (p[pos] < sizeof(tusb_desc_endpoint_t)) return false;
            const tusb_desc_endpoint_t *ep = (const tusb_desc_endpoint_t *)(p + pos);
            if (ep->bmAttributes.xfer == TUSB_XFER_INTERRUPT) {
                if (ep->bEndpointAddress == 0x81 && tu_edpt_packet_size(ep) == INTEL_WIRELESS_SERIES_RX_SIZE) {
                    ep_in = *ep;
                    found_in = true;
                } else if (ep->bEndpointAddress == 0x01 && tu_edpt_packet_size(ep) == 25) {
                    ep_out = *ep;
                    found_out = true;
                }
            }
        }
        pos += p[pos];
    }
    if (!found_in || !found_out) return false;
    memset(r, 0, sizeof(*r));
    r->ep_in = ep_in;
    r->ep_out = ep_out;
    r->opened = true;
    return true;
}

static void alternate_complete(tuh_xfer_t *xfer)
{
    intel_wireless_series_receiver_t *r = receiver(xfer->daddr);
    if (!r || !r->opened) return;
    if (xfer->result == XFER_RESULT_SUCCESS &&
        tuh_edpt_open(xfer->daddr, &r->ep_in) && tuh_edpt_open(xfer->daddr, &r->ep_out)) {
        r->configured = true;
        printf("[intel-wireless-series] Receiver %u: interface 0 alt 1 ready\n", xfer->daddr);
    } else {
        printf("[intel-wireless-series] Receiver %u: alternate setting/endpoint setup failed\n", xfer->daddr);
    }
    usbh_driver_set_config_complete(xfer->daddr, 0);
}

static bool intel_wireless_series_set_config(uint8_t addr, uint8_t itf_num)
{
    intel_wireless_series_receiver_t *r = receiver(addr);
    if (itf_num == 0 && r && r->opened) {
        if (tuh_interface_set(addr, 0, 1, alternate_complete, 0)) return true;
        printf("[intel-wireless-series] Receiver %u: could not submit SET_INTERFACE\n", addr);
    }
    usbh_driver_set_config_complete(addr, itf_num);
    return true;
}

static bool intel_wireless_series_xfer(uint8_t addr, uint8_t ep, xfer_result_t result, uint32_t len)
{
    intel_wireless_series_receiver_t *r = receiver(addr);
    if (!r || !r->configured) return false;
    if (ep == r->ep_in.bEndpointAddress) {
        r->rx_busy = false;
        if (result == XFER_RESULT_SUCCESS) process_packet(addr, r, r->rx, len);
    } else if (ep == r->ep_out.bEndpointAddress) {
        r->tx_busy = false;
        if ((result != XFER_RESULT_SUCCESS || len != INTEL_WIRELESS_SERIES_TX_SIZE) && r->slots[r->tx_slot].gamepad) {
            r->pending_activation |= 1u << r->tx_slot;
        }
    } else {
        return false;
    }
    return true;
}

const usbh_class_driver_t usbh_intel_wireless_series_driver = {
    .name = "Intel Wireless Series", .init = intel_wireless_series_init, .deinit = intel_wireless_series_deinit,
    .open = intel_wireless_series_open, .set_config = intel_wireless_series_set_config,
    .xfer_cb = intel_wireless_series_xfer, .close = intel_wireless_series_disconnect,
};
#endif
