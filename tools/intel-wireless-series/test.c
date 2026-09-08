// Exercise the production driver with real TinyUSB types and mocked USB I/O.
#include <assert.h>
#include <stdlib.h>
#include "../../src/usb/usbh/intel/intel_wireless_series.c"

static input_event_t events[256];
static unsigned event_count, disconnect_count, removed_count, open_count, config_count;
static bool submit_ok = true, claim_ok = true, control_ok = true, endpoint_ok = true;
static unsigned release_count, rx_count, tx_count;
static uint8_t last_tx[6];
static tuh_xfer_cb_t control_cb;
static bool routed[CFG_TUH_DEVICE_MAX + 1][8];

void router_submit_input(const input_event_t *event)
{
    assert(event_count < 256);
    events[event_count++] = *event;
    routed[event->dev_addr][event->instance] = true;
}
void router_device_disconnected(uint8_t addr, int8_t slot)
{
    assert(addr > 0 && slot >= 0 && slot < 8);
    disconnect_count++;
    routed[addr][slot] = false;
}
void remove_players_by_address(int addr, int slot)
{
    assert(addr > 0 && slot >= -1 && slot < 8);
    if (slot == -1) {
        // All output states must be cleared before SHIFT mode can renumber.
        for (unsigned i = 0; i < 8; i++) assert(!routed[addr][i]);
    } else {
        assert(!routed[addr][slot]);
    }
    removed_count++;
}
bool tuh_vid_pid_get(uint8_t addr, uint16_t *vid, uint16_t *pid)
{
    (void)addr;
    *vid = 0x8086;
    *pid = 0xc013;
    return true;
}
bool tuh_edpt_open(uint8_t addr, const tusb_desc_endpoint_t *ep)
{
    (void)addr;
    assert(ep->bmAttributes.xfer == TUSB_XFER_INTERRUPT);
    assert((ep->bEndpointAddress == 0x81 && tu_edpt_packet_size(ep) == 27) ||
           (ep->bEndpointAddress == 1 && tu_edpt_packet_size(ep) == 25));
    open_count++;
    return endpoint_ok;
}
bool tuh_interface_set(uint8_t addr, uint8_t itf, uint8_t alt, tuh_xfer_cb_t cb, uintptr_t data)
{
    (void)addr; (void)data;
    assert(itf == 0 && alt == 1);
    control_cb = cb;
    return control_ok;
}
void usbh_driver_set_config_complete(uint8_t addr, uint8_t itf)
{
    (void)addr; (void)itf;
    config_count++;
}
bool usbh_edpt_claim(uint8_t addr, uint8_t ep)
{
    (void)addr; (void)ep;
    return claim_ok;
}
bool usbh_edpt_release(uint8_t addr, uint8_t ep)
{
    (void)addr; (void)ep;
    release_count++;
    return true;
}
bool usbh_edpt_xfer_with_callback(uint8_t addr, uint8_t ep, uint8_t *buf,
                                uint16_t len, tuh_xfer_cb_t cb, uintptr_t data)
{
    (void)addr; (void)cb; (void)data;
    if (!submit_ok) return false;
    if (ep == 0x81) {
        assert(len == 27);
        rx_count++;
    } else {
        assert(ep == 1 && len == 6);
        memcpy(last_tx, buf, len);
        tx_count++;
    }
    return true;
}

// Interface 0 descriptors, including boot HID and vendor alternate, from
// the connected receiver. The control endpoint must never be opened.
static const uint8_t descriptors[] = {
    9,4,0,0,1,3,1,1,0,
    9,0x21,0,1,0x21,1,0x22,0x3f,0,
    7,5,0x81,3,8,0,10,
    9,4,0,1,3,0,0,0,0,
    7,5,0x81,3,27,0,1,
    7,5,1,3,25,0,1,
    7,5,2,0,8,0,0,
};

static void mount_receiver(uint8_t addr)
{
    assert(intel_wireless_series_open(0, addr, (const tusb_desc_interface_t *)descriptors, sizeof(descriptors)));
    unsigned before = rx_count;
    assert(intel_wireless_series_set_config(addr, 0));
    intel_wireless_series_task();
    assert(rx_count == before); // No polling until SET_INTERFACE succeeds.
    tuh_xfer_t xfer = {.daddr = addr, .result = XFER_RESULT_SUCCESS};
    control_cb(&xfer);
    assert(receivers[addr].configured);
    assert(intel_wireless_series_set_config(addr, 1));
}

static void receive_packet(uint8_t addr, const uint8_t *p, size_t len)
{
    assert(len <= sizeof(receivers[addr].rx));
    memset(receivers[addr].rx, 0xa5, sizeof(receivers[addr].rx));
    memcpy(receivers[addr].rx, p, len);
    assert(intel_wireless_series_xfer(addr, 0x81, XFER_RESULT_SUCCESS, len));
}

int main(void)
{
    assert(intel_wireless_series_init());
    for (size_t n = 0; n < 48; n++) {
        assert(!intel_wireless_series_open(0, 1, (const tusb_desc_interface_t *)descriptors, n));
    }
    uint8_t bad[sizeof(descriptors)];
    memcpy(bad, descriptors, sizeof(bad));
    bad[25] = 0; // Zero-length alternate descriptor, no infinite loop.
    assert(!intel_wireless_series_open(0, 1, (const tusb_desc_interface_t *)bad, sizeof(bad)));
    assert(!intel_wireless_series_open(0, 0, (const tusb_desc_interface_t *)descriptors, sizeof(descriptors)));
    mount_receiver(1);
    assert(open_count == 2 && config_count == 2);
    intel_wireless_series_task();
    assert(rx_count == 1 && tx_count == 0);

    // Actual Mac capture: two remembered slots, activation, zero, ready, B.
    const uint8_t remembered1[] = {3,1,12,2,255,0,0};
    const uint8_t remembered2[] = {3,2,12,2,255,0,0};
    const uint8_t activation[] = {3,3,1,2,255,0,0};
    const uint8_t zero[] = {6,3,0,0,255,0,0};
    const uint8_t ready[] = {3,3,4,2,255,0,0};
    uint8_t input[] = {1,0x17,0x13,0,3,3,6,0x63,4,0,0,2,0};
    receive_packet(1, remembered1, sizeof(remembered1));
    receive_packet(1, remembered2, sizeof(remembered2));
    assert(event_count == 0 && tx_count == 0);
    receive_packet(1, activation, sizeof(activation));
    intel_wireless_series_task();
    const uint8_t expected[] = {3,1,255,0,1,99};
    assert(tx_count == 1 && memcmp(last_tx, expected, 6) == 0);
    assert(intel_wireless_series_xfer(1, 1, XFER_RESULT_SUCCESS, 6));
    receive_packet(1, zero, sizeof(zero));
    receive_packet(1, ready, sizeof(ready));
    assert(event_count == 0);
    receive_packet(1, input, sizeof(input));
    assert(event_count == 1 && events[0].buttons == JP_BUTTON_B2);
    assert(events[0].instance == 3 && events[0].dev_addr == 1);
    assert(events[0].analog[1] == 128);

    // Every truncation must be ignored, even with stale bytes in RX memory.
    unsigned before = event_count;
    for (unsigned n = 0; n < sizeof(input); n++) receive_packet(1, input, n);
    assert(event_count == before);
    for (unsigned n = 0; n < 4; n++) receive_packet(1, activation, n);
    assert(receivers[1].pending_activation == 0);

    static const uint32_t mapped[] = {
        JP_BUTTON_B1,JP_BUTTON_B2,JP_BUTTON_R1,JP_BUTTON_B3,
        JP_BUTTON_B4,JP_BUTTON_L1,JP_BUTTON_L2,JP_BUTTON_R2,
        JP_BUTTON_S2,JP_BUTTON_A1,JP_BUTTON_S1,
    };
    for (unsigned b = 0; b < 11; b++) {
        input[11] = b < 8 ? 1u << b : 0;
        input[12] = b >= 8 ? 1u << (b - 8) : 0;
        receive_packet(1, input, sizeof(input));
        assert(events[event_count-1].buttons == mapped[b]);
    }
    input[11] = input[12] = 0;
    input[9] = input[10] = 0x81;
    receive_packet(1, input, sizeof(input));
    assert(events[event_count-1].buttons == (JP_BUTTON_DL | JP_BUTTON_DU));
    input[9] = input[10] = 0x7f;
    receive_packet(1, input, sizeof(input));
    assert(events[event_count-1].buttons == (JP_BUTTON_DR | JP_BUTTON_DD));
    receive_packet(1, zero, sizeof(zero));
    assert(events[event_count-1].buttons == 0);

    // Multiple queued activations must not overwrite an in-flight reply.
    receive_packet(1, activation, sizeof(activation));
    intel_wireless_series_task();
    uint8_t other[] = {3,7,1,2};
    receive_packet(1, other, sizeof(other));
    intel_wireless_series_task();
    assert(memcmp(receivers[1].tx, expected, 6) == 0);
    assert(intel_wireless_series_xfer(1, 1, XFER_RESULT_SUCCESS, 6));
    intel_wireless_series_task();
    assert(last_tx[0] == 7);
    assert(intel_wireless_series_xfer(1, 1, XFER_RESULT_FAILED, 0));
    intel_wireless_series_task();
    assert(last_tx[0] == 7); // Failed activation is retried.
    assert(intel_wireless_series_xfer(1, 1, XFER_RESULT_SUCCESS, 6));

    // Receiver addresses and all eight slots are independent.
    mount_receiver(2);
    receive_packet(2, activation, sizeof(activation));
    input[4] = 0xfb; // Mask high slot bits.
    receive_packet(2, input, sizeof(input));
    assert(events[event_count-1].dev_addr == 2 && events[event_count-1].instance == 3);
    input[4] = 7;
    receive_packet(1, input, sizeof(input));
    assert(events[event_count-1].instance == 7);
    before = event_count;
    input[4] = 6; // Unknown slot may not create a player.
    receive_packet(1, input, sizeof(input));
    assert(event_count == before);
    other[3] = 0; // Slot 7 becomes a keyboard, clearing its gamepad.
    receive_packet(1, other, sizeof(other));
    assert(!receivers[1].slots[7].gamepad);
    assert(!receivers[1].slots[7].submitted);
    assert(receivers[2].slots[3].submitted);

    // USB transfer failures do not parse stale input; rejected submissions
    // release the claim and can be retried by the next task iteration.
    before = event_count;
    intel_wireless_series_xfer(1, 0x81, XFER_RESULT_FAILED, 13);
    assert(event_count == before);
    submit_ok = false;
    before = release_count;
    intel_wireless_series_task();
    assert(release_count > before && !receivers[1].rx_busy);
    submit_ok = true;
    intel_wireless_series_task();
    assert(receivers[1].rx_busy);

    // Several active RF slots must all clear before bulk player removal.
    for (uint8_t slot = 0; slot < 8; slot++) {
        uint8_t slot_ready[] = {3, slot, 4};
        receive_packet(1, slot_ready, sizeof(slot_ready));
        input[4] = slot;
        receive_packet(1, input, sizeof(input));
    }
    before = disconnect_count;
    intel_wireless_series_disconnect(1); // global unmount callback runs first
    assert(disconnect_count == before + 8);
    assert(!receivers[1].opened && !receivers[1].pending_activation);
    assert(receivers[2].slots[3].submitted);
    before = removed_count;
    usbh_intel_wireless_series_driver.close(1); // subsequent class close is idempotent
    assert(removed_count == before);
    intel_wireless_series_disconnect(2);
    assert(disconnect_count == 11 && removed_count == 4);

    // Failed alternate selection must finish enumeration without polling.
    assert(intel_wireless_series_open(0, 1, (const tusb_desc_interface_t *)descriptors, sizeof(descriptors)));
    intel_wireless_series_set_config(1, 0);
    tuh_xfer_t failed = {.daddr = 1, .result = XFER_RESULT_FAILED};
    control_cb(&failed);
    assert(!receivers[1].configured);
    intel_wireless_series_disconnect(1);
    assert(intel_wireless_series_deinit());
    puts("Intel Wireless Series transport, captured reports, mapping and lifecycle tests passed");
}
