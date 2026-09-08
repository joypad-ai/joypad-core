// p5general_host.c - P5General dongle relay (USB host side)
// SPDX-License-Identifier: MIT
//
// Ported from GP2040-CE P5GeneralAuthUSBListener (MIT, (c) 2024 OpenStickCommunity).
// Detects the P5General dongle (VID 0x2B81/PID 0x0101) on the USB host port and:
//   - signs each report: send hash_pending_buffer OUT, the dongle's IN report IS
//     the signed report (report_received -> hash_finish_buffer);
//   - relays F0/F1/F2 auth via feature reports (host_set/get_report).
// The device side (p5general_mode.c) shares p5general_auth_data in RAM.

#include "p5general_host.h"
#include "platform/platform.h"
#include "tusb.h"
#include <stdio.h>
#include <string.h>

#define P5G_F2_DELAY_MS 500

static uint8_t  ps_dev_addr = 0xFF;
static uint8_t  ps_instance = 0xFF;
static uint8_t  f1_num = 0;
static uint8_t  report_buffer[64];

// The shared auth state is normally owned by the device-side p5general_mode.c
// (a single instance in RAM shared between the host relay and the device mode).
// Console-output builds (Nuon, Dreamcast, 3DO, NeoGeo, UART, Jaguar, custom
// controllers) link this host driver without any USB device mode, so provide a
// weak fallback definition here. When p5general_mode.c IS linked, its strong
// definition overrides this and the single shared instance is preserved.
__attribute__((weak)) p5general_auth_data_t p5general_auth_data = {0};

// ---------------------------------------------------------------------------
// Mount / unmount — self-gates on the dongle VID/PID.
// ---------------------------------------------------------------------------
void p5general_host_mount(uint8_t dev_addr, uint8_t instance)
{
    if (p5general_auth_data.dongle_ready) return;
    uint16_t vid = 0, pid = 0;
    tuh_vid_pid_get(dev_addr, &vid, &pid);
    if (vid == P5GENERAL_VID && pid == P5GENERAL_PID) {
        ps_dev_addr = dev_addr;
        ps_instance = instance;
        f1_num = 0;
        p5general_auth_data.dongle_ready = true;
        p5general_auth_data.passthrough_state = P5G_AUTH_IDLE;
        printf("[P5General] dongle mounted at %d:%d\n", dev_addr, instance);
    }
}

void p5general_host_unmount(uint8_t dev_addr)
{
    if (!p5general_auth_data.dongle_ready || dev_addr != ps_dev_addr) return;
    ps_dev_addr = 0xFF;
    ps_instance = 0xFF;
    p5general_auth_data.dongle_ready = false;
    p5general_auth_data.hash_pending = false;
    p5general_auth_data.hash_ready = false;
    p5general_auth_data.passthrough_state = P5G_AUTH_IDLE;
    printf("[P5General] dongle unmounted\n");
}

bool p5general_host_is_dongle(uint8_t dev_addr)
{
    return p5general_auth_data.dongle_ready && dev_addr == ps_dev_addr;
}

// ---------------------------------------------------------------------------
// The dongle's interrupt-IN report IS the signed report.
// ---------------------------------------------------------------------------
void p5general_host_report_received(uint8_t dev_addr, uint8_t instance,
                                    const uint8_t* report, uint16_t len)
{
    (void)instance;
    if (!p5general_host_is_dongle(dev_addr)) return;
    if (!p5general_auth_data.hash_ready) {
        uint16_t n = len < sizeof(p5general_auth_data.hash_finish_buffer)
                         ? len : sizeof(p5general_auth_data.hash_finish_buffer);
        memcpy(p5general_auth_data.hash_finish_buffer, report, n);
        p5general_auth_data.hash_ready = true;
    }
}

static bool host_get_report(uint8_t report_id, void* buf, uint16_t len)
{
    return tuh_hid_get_report(ps_dev_addr, ps_instance, report_id,
                              HID_REPORT_TYPE_FEATURE, buf, len);
}

static bool host_set_report(uint8_t report_id, void* buf, uint16_t len)
{
    return tuh_hid_set_report(ps_dev_addr, ps_instance, report_id,
                              HID_REPORT_TYPE_FEATURE, buf, len);
}

// ---------------------------------------------------------------------------
// Per-loop task — push pending report to dongle + drive F0/F1/F2.
// ---------------------------------------------------------------------------
void p5general_host_task(void)
{
    p5general_auth_data_t* a = &p5general_auth_data;
    if (!a->dongle_ready) return;

    // Push a report to sign over the interrupt OUT endpoint.
    if (a->hash_pending && tuh_hid_send_ready(ps_dev_addr, ps_instance)) {
        tuh_hid_send_report(ps_dev_addr, ps_instance, 0, a->hash_pending_buffer, 64);
        a->hash_pending = false;
    }

    switch (a->passthrough_state) {
        case P5G_AUTH_SEND_F0:
            memcpy(report_buffer, a->auth_buffer, 64);
            host_set_report(P5GENERAL_SET_AUTH_PAYLOAD, report_buffer, 64);
            a->passthrough_state = P5G_AUTH_SEND_F0_WAIT;
            break;
        case P5G_AUTH_RECV_F1:
            if (f1_num) {
                host_get_report(P5GENERAL_GET_SIGNATURE_NONCE, report_buffer, 64);
                a->passthrough_state = P5G_AUTH_RECV_F1_WAIT;
                f1_num--;
            } else {
                a->passthrough_state = P5G_AUTH_IDLE;
            }
            break;
        case P5G_AUTH_RECV_F2_DELAY:
            if (platform_time_ms() >= a->auth_recv_f2_us) {
                a->passthrough_state = P5G_AUTH_RECV_F2;
            } else {
                break;
            }
            // fallthrough
        case P5G_AUTH_RECV_F2:
            host_get_report(P5GENERAL_GET_SIGNING_STATE, report_buffer, 16);
            a->passthrough_state = P5G_AUTH_RECV_F2_WAIT;
            break;
        default:
            break;
    }
}

// ---------------------------------------------------------------------------
// Completion hooks — invoked from the global cbs in sony_ds4.c.
// ---------------------------------------------------------------------------
bool p5general_host_on_set_report_complete(uint8_t dev_addr, uint8_t instance,
                                           uint8_t report_id, uint16_t len)
{
    p5general_auth_data_t* a = &p5general_auth_data;
    if (!a->dongle_ready || dev_addr != ps_dev_addr || instance != ps_instance) return false;
    (void)len;

    if (report_id == P5GENERAL_SET_AUTH_PAYLOAD &&
        a->passthrough_state == P5G_AUTH_SEND_F0_WAIT) {
        switch (a->auth_buffer[1]) {
            case 0x01: f1_num = 4; break;
            case 0x03: f1_num = 1; break;
            case 0x02: default: f1_num = 0; break;
        }
        if (((a->auth_buffer[1] == 0x01) && (a->auth_buffer[3] == 3)) ||
            (a->auth_buffer[1] == 0x02) || (a->auth_buffer[1] == 0x03)) {
            a->auth_recv_f2_us = platform_time_ms() + P5G_F2_DELAY_MS;
            a->passthrough_state = P5G_AUTH_RECV_F2_DELAY;
        } else {
            a->passthrough_state = P5G_AUTH_IDLE;
        }
    }
    return true;
}

bool p5general_host_on_get_report_complete(uint8_t dev_addr, uint8_t instance,
                                           uint8_t report_id, uint16_t len)
{
    p5general_auth_data_t* a = &p5general_auth_data;
    if (!a->dongle_ready || dev_addr != ps_dev_addr || instance != ps_instance) return false;

    switch (report_id) {
        case P5GENERAL_GET_SIGNATURE_NONCE:
            if (a->passthrough_state == P5G_AUTH_RECV_F1_WAIT) {
                uint16_t n = len < sizeof(a->auth_buffer) ? len : sizeof(a->auth_buffer);
                memcpy(a->auth_buffer, report_buffer, n);
                a->passthrough_state = P5G_AUTH_IDLE;
            }
            break;
        case P5GENERAL_GET_SIGNING_STATE:
            if (a->passthrough_state == P5G_AUTH_RECV_F2_WAIT) {
                uint16_t n = len < sizeof(a->auth_buffer) ? len : sizeof(a->auth_buffer);
                memcpy(a->auth_buffer, report_buffer, n);
                a->passthrough_state = P5G_AUTH_IDLE;
            }
            break;
        default:
            break;
    }
    return true;
}
