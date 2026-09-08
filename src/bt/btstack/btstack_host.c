// btstack_host.c - BTstack HID Host (BLE + Classic)
//
// Transport-agnostic BTstack integration for HID devices.
// Uses BTstack's SM (Security Manager) for LE Secure Connections,
// GATT client for HID over GATT Profile (HOGP), and
// HID Host for Classic BT HID devices.

#include "btstack_host.h"
#ifdef CONFIG_BT_CLASSIC_OUTPUT
#include "ble_output/ble_output.h"   // ble_output_get_mode() / BLE_MODE_SWITCH_BT
#include "switch_bt/switch_bt.h"     // switch_bt_apply_gap_identity()
// True when this build is acting as a Switch Pro Controller (HID device role). In
// that mode the BT-HID *host* logic must stand down on incoming connections — the
// Switch is connecting to OUR device role, not us hosting a controller.
static inline bool bh_switch_bt_mode(void) { return ble_output_get_mode() == BLE_MODE_SWITCH_BT; }
#else
static inline bool bh_switch_bt_mode(void) { return false; }
#endif

#ifdef BTSTACK_DEFER_SCAN
static bool btstack_host_scan_enabled = false;
void btstack_host_enable_scan(void) {
    btstack_host_scan_enabled = true;
    btstack_host_start_scan();
}
#endif
#include "btstack_config.h"
#include "bt_device_db.h"
// Include specific BTstack headers instead of umbrella btstack.h
// (btstack.h pulls in audio codecs which need sbc_encoder.h)
#include "btstack_defines.h"
#include "btstack_event.h"
#include "btstack_run_loop.h"

// Run loop depends on transport: embedded for USB dongle, async_context for CYW43,
// FreeRTOS for ESP32
#if !defined(BTSTACK_USE_CYW43) && !defined(BTSTACK_USE_ESP32) && !defined(BTSTACK_USE_NRF)
#include "btstack_run_loop_embedded.h"
#endif

// Declare btstack_memory_init - can't include btstack_memory.h due to HID conflicts
extern void btstack_memory_init(void);

#include "bluetooth_data_types.h"
#include "bluetooth_company_id.h"
#include "bluetooth_sdp.h"
#include "ad_parser.h"
#include "gap.h"
#include "hci.h"
#include "l2cap.h"
#include "ble/sm.h"
#include "ble/gatt_client.h"
#include "ble/att_db_util.h"
#include "ble/att_server.h"
#include "ble/le_device_db.h"
#include "ble/gatt-service/hids_client.h"
#include "ble/gatt-service/device_information_service_client.h"
#include "ble/gatt-service/battery_service_client.h"
#include "classic/hid_host.h"
#include "classic/sdp_client.h"
#include "classic/sdp_server.h"
#include "classic/sdp_util.h"
#include "classic/device_id_server.h"

// Link key storage: TLV (flash) based for all builds
// USB dongle uses pico_flash_bank_instance(), CYW43/ESP32 use their own TLV setup
#if !defined(BTSTACK_USE_CYW43) && !defined(BTSTACK_USE_ESP32) && !defined(BTSTACK_USE_NRF)
#include "classic/btstack_link_key_db_tlv.h"
#include "ble/le_device_db_tlv.h"
#include "btstack_tlv_flash_bank.h"
#include "pico/btstack_flash_bank.h"
#include "pico/flash.h"
#include "hardware/flash.h"
#endif

#include "btstack_tlv.h"
#include "hci_dump.h"
#include "hci_dump_embedded_stdout.h"

// BTHID callbacks - for classic BT HID devices
extern void bt_on_hid_ready(uint8_t conn_index);
extern void bt_on_disconnect(uint8_t conn_index);
extern void bt_on_hid_report(uint8_t conn_index, const uint8_t* data, uint16_t len);
extern void bthid_update_device_info(uint8_t conn_index, const char* name,
                                      uint16_t vendor_id, uint16_t product_id);
extern void bthid_set_battery_level(uint8_t conn_index, uint8_t level);
extern void bthid_set_battery(uint8_t conn_index, uint8_t level, bool charging);
extern void bthid_set_hid_descriptor(uint8_t conn_index, const uint8_t* desc, uint16_t desc_len);

// Platform HAL
extern void platform_reboot(void);

#include <stdio.h>
#include <string.h>

// For rumble feedback passthrough
// Note: manager.h includes tusb.h which conflicts with BTstack, so forward declare
extern int find_player_index(int dev_addr, int instance);
#include "core/services/players/feedback.h"

// ============================================================================
// FLASH HELPERS (for TLV storage)
// ============================================================================
#if !defined(BTSTACK_USE_CYW43) && !defined(BTSTACK_USE_ESP32) && !defined(BTSTACK_USE_NRF)
// Erase both BTstack flash banks (8KB total at end of flash)
static void __no_inline_not_in_flash_func(flash_erase_banks_func)(void* p) {
    (void)p;
    uint32_t flash_offset = PICO_FLASH_SIZE_BYTES - (FLASH_SECTOR_SIZE * 2);
    // Erase both 4KB sectors
    flash_range_erase(flash_offset, FLASH_SECTOR_SIZE * 2);
}

// Erase BTstack flash banks using flash_safe_execute
static void btstack_erase_flash_banks(void) {
    printf("[BTSTACK_HOST] Erasing BTstack flash banks at 0x%lX...\n",
           (unsigned long)(PICO_FLASH_SIZE_BYTES - (FLASH_SECTOR_SIZE * 2)));
    int result = flash_safe_execute(flash_erase_banks_func, NULL, UINT32_MAX);
    if (result == PICO_OK) {
        printf("[BTSTACK_HOST] Flash banks erased successfully\n");
    } else {
        printf("[BTSTACK_HOST] Flash erase failed: %d\n", result);
    }
}
#endif

// ============================================================================
// BLE HID REPORT ROUTING
// ============================================================================

// Deferred processing to avoid stack overflow in BTstack callback
static uint8_t pending_ble_report[64];  // 64 bytes for Switch 2 reports
static uint16_t pending_ble_report_len = 0;
static uint8_t pending_ble_conn_index = 0;
static volatile bool ble_report_pending = false;

// Forward declare the function to route BLE reports through bthid layer
static void route_ble_hid_report(uint8_t conn_index, const uint8_t* data, uint16_t len);

// Forward declare Switch 2 functions (defined later with state machine)
static void switch2_retry_init_if_needed(void);
static void switch2_handle_feedback(void);

// ============================================================================
// CONFIGURATION
// ============================================================================

#define MAX_BLE_CONNECTIONS 2
#ifndef SCAN_INTERVAL
#define SCAN_INTERVAL 0x00A0  // 100ms (default)
#endif
#ifndef SCAN_WINDOW
#define SCAN_WINDOW   0x0050  // 50ms (default)
#endif

// ============================================================================
// STATE
// ============================================================================

typedef enum {
    BLE_STATE_IDLE,
    BLE_STATE_SCANNING,
    BLE_STATE_CONNECTING,
    BLE_STATE_CONNECTED,
    BLE_STATE_DISCOVERING,
    BLE_STATE_READY
} ble_state_t;

typedef struct {
    bd_addr_t addr;
    bd_addr_type_t addr_type;
    hci_con_handle_t handle;
    ble_state_t state;

    // GATT discovery state
    uint16_t hid_service_start;
    uint16_t hid_service_end;
    uint16_t report_char_handle;
    uint16_t report_ccc_handle;

    // Device info
    char name[48];
    const bt_device_profile_t* profile;
    uint16_t vid;
    uint16_t pid;

    // Connection index for bthid layer (offset by MAX_CLASSIC_CONNECTIONS)
    uint8_t conn_index;
    bool hid_ready;

    // Per-connection BLE HID client id. MUST be per-connection (not a single
    // global) so two BLE HID devices route reports + descriptors independently —
    // a shared cid cross-wires their reports/descriptors -> garbage.
    uint16_t hids_cid;

    // Identify-stall watchdog: when a link comes up unidentified (no name match,
    // generic HOGP path) and the generic HIDS bring-up never reaches the point
    // that reads DIS, VID/PID stay 0 and a name-less SC2 is stuck as a generic
    // gamepad with no input. connect_ms lets a periodic task, after a grace
    // window, force a DIS read so the DIS-reactive Valve switch can fire.
    uint32_t connect_ms;      // when the LE link connected (0 = slot unused)
    bool dis_probe_started;   // proactive DIS query already issued for this link
} ble_connection_t;

// BLE conn_index offset (BLE devices use conn_index >= this value)
#define BLE_CONN_INDEX_OFFSET MAX_CLASSIC_CONNECTIONS

typedef enum {
    GATT_IDLE,
    GATT_DISCOVERING_SERVICES,
    GATT_DISCOVERING_HID_CHARACTERISTICS,
    GATT_ENABLING_NOTIFICATIONS,
    GATT_READY
} gatt_state_t;

static struct {
    bool initialized;
    bool powered_on;
    ble_state_t state;

    // HCI transport (provided by caller)
    const hci_transport_t* hci_transport;

    // Scanning
    bool scan_active;

    // Pending connection
    bd_addr_t pending_addr;
    bd_addr_type_t pending_addr_type;
    char pending_name[48];
    const bt_device_profile_t* pending_profile;
    uint16_t pending_vid;
    uint16_t pending_pid;

    // Last connected device (for reconnection)
    bd_addr_t last_connected_addr;
    bd_addr_type_t last_connected_addr_type;
    char last_connected_name[48];
    bool has_last_connected;
    uint32_t reconnect_attempt_time;
    uint8_t reconnect_attempts;
    uint32_t scan_start_time;          // When current scan started (for periodic reconnect)

    // Connections
    ble_connection_t connections[MAX_BLE_CONNECTIONS];

    // GATT discovery state
    gatt_state_t gatt_state;
    hci_con_handle_t gatt_handle;
    uint16_t hid_service_start;
    uint16_t hid_service_end;
    gatt_client_characteristic_t report_characteristic;  // Full HID Report characteristic

    // Callbacks
    btstack_host_report_callback_t report_callback;
    btstack_host_connect_callback_t connect_callback;

    // HIDS Client cid is now PER-CONNECTION (ble_connection_t.hids_cid) so two
    // BLE HID devices don't cross-wire. (Removed the single global hids_cid.)

    // Battery Service Client
    uint16_t bas_cid;

} hid_state;

// HID descriptor storage (shared across connections)
static uint8_t hid_descriptor_storage[1024];  // room for 2 BLE HID descriptors (MAX_NR_HIDS_CLIENTS=2)

static btstack_packet_callback_registration_t hci_event_callback_registration;
static btstack_packet_callback_registration_t sm_event_callback_registration;

// Direct notification listener for Xbox HID reports (bypasses HIDS client)
static gatt_client_notification_t xbox_hid_notification_listener;
static gatt_client_characteristic_t xbox_hid_characteristic;  // Fake characteristic for listener
static void xbox_hid_notification_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);

// Direct notification listener for Switch 2 HID reports
static gatt_client_notification_t switch2_hid_notification_listener;
static gatt_client_characteristic_t switch2_hid_characteristic;
static void switch2_hid_notification_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);

// Forward declaration for BLE disconnect cleanup (defined in Switch 2 section)
static void switch2_cleanup_on_disconnect(void);

// ============================================================================
// CLASSIC BT HID HOST STATE
// ============================================================================

#define MAX_CLASSIC_CONNECTIONS 4
#define INQUIRY_DURATION 5  // Inquiry duration in 1.28s units
// Number of active Classic links. Distinct from
// btstack_classic_get_connection_count(), which deliberately sums Classic AND
// BLE for LED/status purposes; decisions about Classic scanning must not be
// influenced by an unrelated BLE peer.
static uint8_t classic_link_count(void);

#define CLASSIC_CONNECT_TIMEOUT_MS 15000  // Max time to establish HID connection

typedef struct {
    bool active;
    uint16_t hid_cid;           // BTstack HID connection ID
    bd_addr_t addr;
    char name[48];
    uint8_t class_of_device[3];
    uint16_t vendor_id;
    uint16_t product_id;
    bool hid_ready;
    const bt_device_profile_t* profile;
    uint32_t connect_time;      // When connection was initiated (for timeout detection)
} classic_connection_t;

#ifdef CONFIG_DS5_DROP_SCREAM
#include "pico.h"             // __not_in_flash_func for the RAM-resident tap
#include "hardware/watchdog.h"
#include "hardware/structs/watchdog.h"

// Hard-fault black box: stash the faulting PC/LR in watchdog scratch
// registers (survive reboot) and reset. Reported ~5s after next boot so a
// reattached log listener catches it. scratch[4] = magic, [5] = PC, [6] = LR.
#define DS5_CRASH_MAGIC 0xDEADFA11u

void __not_in_flash_func(isr_hardfault)(void)
{
    uint32_t* sp;
    __asm volatile ("mrs %0, msp" : "=r"(sp));
    watchdog_hw->scratch[4] = DS5_CRASH_MAGIC;
    watchdog_hw->scratch[5] = sp[6];   // stacked PC
    watchdog_hw->scratch[6] = sp[5];   // stacked LR
    watchdog_reboot(0, 0, 10);
    while (1) { __asm volatile ("nop"); }
}

static uint32_t crash_report_pc, crash_report_lr;
static bool crash_report_pending;

// Queryable anytime via BT.STATUS ("crash_pc"): a one-shot boot print gets
// missed when no log listener is attached at the time.
void btstack_host_get_crash_info(uint32_t* pc, uint32_t* lr)
{
    *pc = crash_report_pc;
    *lr = crash_report_lr;
}
// HID interrupt-channel CIDs for direct l2cap_send() (DS5 audio streaming).
// BTstack delivers L2CAP_EVENT_CHANNEL_OPENED only to the owning service
// (hid_host), so we observe it via the public hci_dump interface instead:
// l2cap_emit_channel_opened() feeds every event through hci_dump (with
// ENABLE_LOG_BTSTACK_EVENTS) before dispatching. No BTstack modification.
#include "hci_dump.h"

static struct {
    bd_addr_t addr;
    uint16_t cid;
} hid_intr_cids[4];  // MAX_CLASSIC_CONNECTIONS

static void __not_in_flash_func(ds5_cid_tap_reset)(void) {}
static void __not_in_flash_func(ds5_cid_tap_log_message)(int log_level, const char* format,
                                                         va_list argptr)
{
    (void)log_level; (void)format; (void)argptr;
}
// RAM-resident and self-contained: this callback fires for every HCI event,
// including during BTstack link-key FLASH writes (pairing/auth). A
// flash-resident function executing in that window can hard-fault the chip
// (same class of bug as the Core1 flash-contention issues elsewhere in this
// repo) — the adapter rebooted mid-handshake. No printf, no libc, no
// flash-resident callees in here.
static void __not_in_flash_func(ds5_cid_tap_log_packet)(uint8_t packet_type, uint8_t in,
                                                        uint8_t* packet, uint16_t len)
{
    (void)in;
    if (packet_type != HCI_EVENT_PACKET || len < 4) return;

    if (packet[0] == L2CAP_EVENT_CHANNEL_OPENED && len >= 24) {
        // [2]=status [3..8]=addr(reversed) [11..12]=psm [13..14]=local_cid
        if (packet[2] != 0) return;
        uint16_t psm = (uint16_t)(packet[11] | (packet[12] << 8));
        if (psm != PSM_HID_INTERRUPT) return;
        uint16_t cid = (uint16_t)(packet[13] | (packet[14] << 8));
        uint8_t addr[6];
        for (int b = 0; b < 6; b++) addr[b] = packet[3 + 5 - b];
        int free_slot = -1;
        for (int ci = 0; ci < 4; ci++) {
            bool same = true;
            for (int b = 0; b < 6; b++) {
                if (hid_intr_cids[ci].addr[b] != addr[b]) { same = false; break; }
            }
            if (same) {
                hid_intr_cids[ci].cid = cid;
                return;
            }
            if (free_slot < 0 && hid_intr_cids[ci].cid == 0) free_slot = ci;
        }
        if (free_slot >= 0) {
            for (int b = 0; b < 6; b++) hid_intr_cids[free_slot].addr[b] = addr[b];
            hid_intr_cids[free_slot].cid = cid;
        }
    } else if (packet[0] == L2CAP_EVENT_CHANNEL_CLOSED) {
        // [2..3]=local_cid
        uint16_t cid = (uint16_t)(packet[2] | (packet[3] << 8));
        for (int ci = 0; ci < 4; ci++) {
            if (hid_intr_cids[ci].cid == cid) {
                hid_intr_cids[ci].cid = 0;
                for (int b = 0; b < 6; b++) hid_intr_cids[ci].addr[b] = 0;
            }
        }
    }
}

static const hci_dump_t ds5_cid_tap = {
    .reset = ds5_cid_tap_reset,
    .log_packet = ds5_cid_tap_log_packet,
    .log_message = ds5_cid_tap_log_message,
};

#endif

static struct {
    bool inquiry_active;
    bool use_liac;  // Alternate between GIAC and LIAC for Wiimote/Wii U Pro discovery
    classic_connection_t connections[MAX_CLASSIC_CONNECTIONS];
    // Pending incoming connection info (from HCI_EVENT_CONNECTION_REQUEST)
    bd_addr_t pending_addr;
    uint32_t pending_cod;
    char pending_name[48];
    uint16_t pending_vid;
    uint16_t pending_pid;
    bool pending_valid;
    bool pending_outgoing;  // True if we initiated the connection (hid_host_connect)
    hci_con_handle_t pending_acl_handle;  // ACL handle for pending incoming connection
    const bt_device_profile_t* pending_profile;
    // Pending HID connect (deferred until encryption completes)
    bd_addr_t pending_hid_addr;
    hci_con_handle_t pending_hid_handle;
    bool pending_hid_connect;
    // Set after outgoing HID fails — wait for device to reconnect incoming
    uint32_t waiting_for_incoming_time;  // 0 = not waiting
    // Connection timeout recovery
    uint32_t recovery_start_time;        // When recovery started (0 = no recovery pending)
} classic_state;

// ============================================================================
// WIIMOTE DIRECT L2CAP STATE
// ============================================================================
// Wiimotes don't work well with BTstack's hid_host layer.
// We bypass it and create L2CAP channels directly, like USB Host Shield does.

#ifndef PSM_HID_CONTROL
#define PSM_HID_CONTROL   0x0011
#endif
#ifndef PSM_HID_INTERRUPT
#define PSM_HID_INTERRUPT 0x0013
#endif

typedef enum {
    WIIMOTE_STATE_IDLE,
    WIIMOTE_STATE_W4_CONTROL_CONNECTED,
    WIIMOTE_STATE_W4_INTERRUPT_CONNECTED,
    WIIMOTE_STATE_CONNECTED
} wiimote_state_t;

typedef struct {
    bool active;
    wiimote_state_t state;
    bd_addr_t addr;
    hci_con_handle_t acl_handle;
    uint16_t control_cid;
    uint16_t interrupt_cid;
    char name[48];
    uint8_t class_of_device[3];
    uint16_t vendor_id;
    uint16_t product_id;
    int conn_index;  // Index in classic_state.connections for bthid routing
    bool using_hid_host;  // True if reconnected via HID Host (not direct L2CAP)
    uint16_t hid_host_cid;  // HID Host CID for sending (when using_hid_host is true)
    bool hid_host_ready;  // True when HID Host is ready to send (after DESCRIPTOR_AVAILABLE)
} wiimote_connection_t;

static wiimote_connection_t wiimote_conn;

// Forward declaration
static void wiimote_l2cap_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);

// SDP query state
static uint8_t sdp_attribute_value[32];
static const uint16_t sdp_attribute_value_buffer_size = sizeof(sdp_attribute_value);

// Classic HID descriptor storage
static uint8_t classic_hid_descriptor_storage[512];

// SDP Device ID record buffer (needed for DS4/DS5 reconnection)
static uint8_t device_id_sdp_service_buffer[100];

// Find classic connection by hid_cid
static classic_connection_t* find_classic_connection_by_cid(uint16_t hid_cid) {
    for (int i = 0; i < MAX_CLASSIC_CONNECTIONS; i++) {
        if (classic_state.connections[i].active && classic_state.connections[i].hid_cid == hid_cid) {
            return &classic_state.connections[i];
        }
    }
    return NULL;
}

// Get conn_index for classic connection
static int get_classic_conn_index(uint16_t hid_cid) {
    for (int i = 0; i < MAX_CLASSIC_CONNECTIONS; i++) {
        if (classic_state.connections[i].active && classic_state.connections[i].hid_cid == hid_cid) {
            return i;  // conn_index matches array index
        }
    }
    return -1;
}

// Find free classic connection slot
static classic_connection_t* find_free_classic_connection(void) {
    for (int i = 0; i < MAX_CLASSIC_CONNECTIONS; i++) {
        if (!classic_state.connections[i].active) {
            return &classic_state.connections[i];
        }
    }
    return NULL;
}

// ============================================================================
// BLE CONNECTION HELPERS
// ============================================================================

// Get BLE connection by conn_index
static ble_connection_t* find_ble_connection_by_conn_index(uint8_t conn_index) {
    if (conn_index < BLE_CONN_INDEX_OFFSET) return NULL;
    uint8_t ble_index = conn_index - BLE_CONN_INDEX_OFFSET;
    if (ble_index >= MAX_BLE_CONNECTIONS) return NULL;
    if (hid_state.connections[ble_index].handle == HCI_CON_HANDLE_INVALID) return NULL;
    return &hid_state.connections[ble_index];
}

// Get conn_index for BLE connection
static int get_ble_conn_index_by_handle(hci_con_handle_t handle) {
    for (int i = 0; i < MAX_BLE_CONNECTIONS; i++) {
        if (hid_state.connections[i].handle == handle) {
            return BLE_CONN_INDEX_OFFSET + i;
        }
    }
    return -1;
}

// Route BLE HID report through bthid layer
static void route_ble_hid_report(uint8_t conn_index, const uint8_t* data, uint16_t len)
{
    // Build BTHID-compatible packet: DATA|INPUT header + report
    // Buffer needs to hold 1 byte header + up to 64 bytes of report data
    static uint8_t hid_packet[65];
    hid_packet[0] = 0xA1;  // DATA | INPUT header
    if (len <= sizeof(hid_packet) - 1) {
        memcpy(hid_packet + 1, data, len);
        bt_on_hid_report(conn_index, hid_packet, len + 1);
    }
}

// ============================================================================
// FORWARD DECLARATIONS
// ============================================================================

static void packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);
static void sm_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);
static void hids_client_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);
static void hid_host_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);
static ble_connection_t* find_connection_by_handle(hci_con_handle_t handle);
static ble_connection_t* find_connection_by_hids_cid(uint16_t hids_cid);
static ble_connection_t* find_free_connection(void);
static void start_hids_client(ble_connection_t *conn);
static void register_ble_hid_listener(hci_con_handle_t con_handle);
static void register_switch2_hid_listener(hci_con_handle_t con_handle);
// MouthPad NUS client hooks (defined in the NUS section below)
static void mp_nus_mark_pending(hci_con_handle_t handle);
static void mp_nus_disconnected(hci_con_handle_t handle);
static void mp_nus_periodic(void);
// Valve Steam Controller 2 custom GATT client hooks (defined below)
static void register_valve_hid_listener(hci_con_handle_t con_handle);
static void valve_disconnected(hci_con_handle_t handle);
static void valve_periodic(void);
static void ble_identify_stall_task(void);

// On a bonded reconnect the SC2 (and some others) advertise with NO name, so the
// scan-time bt_device_lookup falls to the generic profile and the connection would
// take the generic HOGP/HIDS path — fatal for the SC2, which needs its Valve GATT
// client (lizard-off) or it stays silent. But conn->name is restored from the stored
// bond (e.g. "Steam Ctrl (BT) ..."), so re-derive the profile from that name here,
// before dispatch, whenever the current profile is unset/generic. Returns the (possibly
// updated) profile so callers can dispatch on its ->ble strategy.
static const bt_device_profile_t* ble_resolve_profile_from_name(ble_connection_t* conn)
{
    if (!conn) return NULL;
    if (conn->profile && conn->profile != &BT_PROFILE_DEFAULT &&
        conn->profile->ble != BT_BLE_NONE) {
        return conn->profile;  // already a specific BLE strategy — keep it
    }
    if (conn->name[0]) {
        const bt_device_profile_t* p = bt_device_lookup_by_name(conn->name);
        if (p && p != &BT_PROFILE_DEFAULT && p->ble != BT_BLE_NONE) {
            printf("[BTSTACK_HOST] reconnect: re-derived profile '%s' from name '%s'\n",
                   p->name, conn->name);
            conn->profile = p;
        }
    }
    return conn->profile;
}

// Deferred post-HID setup sequencer. After HID report notifications are
// enabled (0x1C), the hids_client needs a moment to return to CONNECTED before
// it will accept a protocol-mode write, and the other GATT clients must run one
// at a time. This runs from btstack_host_process(): phase 0 = write REPORT
// protocol mode (retry until accepted), phase 1 = start DIS/BAS + arm NUS.
// Per-connection so two BLE HID devices each get their own REPORT-mode write +
// notification-enable sequence (a single global would only set up the last device).
static struct {
    bool active;
    hci_con_handle_t handle;
    uint16_t hids_cid;
    uint8_t phase;
    uint32_t start_ms;
    uint32_t phase_ms;
} mp_hid_setup[MAX_BLE_CONNECTIONS];
static void mp_hid_setup_task(void);
static void start_battery_service_client(hci_con_handle_t handle);
static void dis_client_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);

// ============================================================================
// INITIALIZATION
// ============================================================================

// Internal function to set up HID handlers (used by both init paths)
// Minimal GATT server for the central. The MouthPad (and other Nordic-based
// HOGP devices built to talk to a companion app) act as a GATT *client* toward
// the host after connecting -- they discover/subscribe on our side. With no ATT
// server answering, that request hangs the full 30s ATT transaction timeout and
// the device terminates the link (reason 0x13) without ever streaming HID.
// Exposing GAP (device name/appearance) + GATT (Service Changed) services makes
// us look like a normal host so the device proceeds to stream.
static uint8_t host_att_device_name[] = "Joypad OS";
static const uint8_t host_att_appearance[] = { 0xC0, 0x03 }; // 0x03C0 Generic HID, LE

static uint16_t host_att_read_callback(hci_con_handle_t con_handle, uint16_t att_handle,
                                       uint16_t offset, uint8_t *buffer, uint16_t buffer_size) {
    (void)con_handle; (void)att_handle; (void)offset; (void)buffer; (void)buffer_size;
    return 0; // static values are served from the DB directly
}

static int host_att_write_callback(hci_con_handle_t con_handle, uint16_t att_handle,
                                   uint16_t transaction_mode, uint16_t offset,
                                   uint8_t *buffer, uint16_t buffer_size) {
    (void)con_handle; (void)att_handle; (void)transaction_mode;
    (void)offset; (void)buffer; (void)buffer_size;
    return 0; // accept (e.g. CCCD writes) so the peer's setup completes
}

// Defined (strong) by ble_output.c when the BLE-peripheral path owns the ATT
// server with its full GATT profile (e.g. controller_btusb, usb2ble). In that
// case we must NOT init a second, minimal server -- it would clobber the rich
// profile. Central-only builds (bt2usb, mouthpad) don't link ble_output, so the
// weak default applies and we install the minimal server.
__attribute__((weak)) bool btstack_host_external_att_server(void) { return false; }

static void setup_att_server(void) {
    if (btstack_host_external_att_server()) {
        printf("[BTSTACK_HOST] ATT server owned externally (ble_output) -- skipping minimal server\n");
        return;
    }
    printf("[BTSTACK_HOST] Init ATT server (minimal GAP+GATT)...\n");
    att_db_util_init();
    // GAP service (0x1800)
    att_db_util_add_service_uuid16(0x1800);
    att_db_util_add_characteristic_uuid16(0x2A00, ATT_PROPERTY_READ,
        ATT_SECURITY_NONE, ATT_SECURITY_NONE,
        host_att_device_name, sizeof(host_att_device_name) - 1);
    att_db_util_add_characteristic_uuid16(0x2A01, ATT_PROPERTY_READ,
        ATT_SECURITY_NONE, ATT_SECURITY_NONE,
        (uint8_t *)host_att_appearance, sizeof(host_att_appearance));
    // GATT service (0x1801) with Service Changed (indicate)
    att_db_util_add_service_uuid16(0x1801);
    att_db_util_add_characteristic_uuid16(0x2A05, ATT_PROPERTY_INDICATE,
        ATT_SECURITY_NONE, ATT_SECURITY_NONE, NULL, 0);
    att_server_init(att_db_util_get_address(), host_att_read_callback, host_att_write_callback);
    printf("[BTSTACK_HOST] ATT server initialized (db size=%u)\n", att_db_util_get_size());
}

static void setup_hid_handlers(void)
{
    printf("[BTSTACK_HOST] Init L2CAP...\n");
    l2cap_init();

    // Raise the LE ATT MTU so large HID input reports fit in a single GATT
    // notification. BLE notifications carry only (MTU-3) bytes; the default MTU
    // of 23 caps that at 20, so a 64-byte SInput report (JoypadOS controllers)
    // would never be delivered — the device connects but sends zero input.
    // 247 covers the full report with margin (fits HCI_ACL_PAYLOAD_SIZE).
    l2cap_set_max_le_mtu(247);

    printf("[BTSTACK_HOST] Init SM...\n");
    sm_init();

    // Configure SM - bonding + LE Secure Connections. Some HOGP devices (e.g.
    // Augmental MouthPad) accept a legacy-paired connection and even accept the
    // report CCC writes, but will NOT stream HID notifications unless the link
    // is secured with LE Secure Connections. Request SC (peers without SC fall
    // back to legacy automatically, so other controllers are unaffected).
    sm_set_io_capabilities(IO_CAPABILITY_NO_INPUT_NO_OUTPUT);
    sm_set_authentication_requirements(SM_AUTHREQ_BONDING | SM_AUTHREQ_SECURE_CONNECTION);
    sm_set_encryption_key_size_range(7, 16);

    printf("[BTSTACK_HOST] Init GATT client...\n");
    gatt_client_init();

    // Actually negotiate the raised ATT MTU: l2cap_set_max_le_mtu() only sets our
    // maximum — the client must send an MTU Exchange Request to use it, and btstack's
    // auto-negotiation is OFF by default. Without this the link stays at the 23-byte
    // default (20-byte notifications), so a controller whose full input report exceeds
    // 20 bytes (original Steam Controller BLE) connects but stalls the moment its
    // report grows past one packet. Enabling this makes us request the larger MTU up
    // front, matching what phones/PCs do.
    gatt_client_mtu_enable_auto_negotiation(1);

    // Minimal ATT server so peers that act as GATT clients toward us don't hang
    // on the 30s ATT timeout (see setup_att_server comment).
    setup_att_server();

    printf("[BTSTACK_HOST] Init HIDS client...\n");
    hids_client_init(hid_descriptor_storage, sizeof(hid_descriptor_storage));

    printf("[BTSTACK_HOST] Init DIS client...\n");
    device_information_service_client_init();

    printf("[BTSTACK_HOST] Init Battery Service client...\n");
    battery_service_client_init();

    printf("[BTSTACK_HOST] Init LE Device DB...\n");
    le_device_db_init();

    // Initialize classic BT HID Host — but NOT in Switch-BT output mode: the HID
    // host registers L2CAP listening services on PSM 0x11/0x13 (control/interrupt),
    // the exact channels our HID *device* role needs. Both can't own them, so the
    // Switch's inbound connection would land on the host handler instead of
    // switch_bt. In Switch-BT mode we emulate a controller (input comes from GPIO/
    // USB), so the host is unnecessary — skip it and let switch_bt own the channels.
    bool skip_hid_host = false;
#ifdef CONFIG_BT_CLASSIC_OUTPUT
    skip_hid_host = (ble_output_get_mode() == BLE_MODE_SWITCH_BT);
#endif
    if (!skip_hid_host) {
        printf("[BTSTACK_HOST] Init Classic HID Host...\n");
        memset(&classic_state, 0, sizeof(classic_state));
        // Set security level BEFORE hid_host_init (it registers L2CAP services with this level)
        gap_set_security_level(LEVEL_0);  // DS3 doesn't support SSP
        hid_host_init(classic_hid_descriptor_storage, sizeof(classic_hid_descriptor_storage));
        hid_host_register_packet_handler(hid_host_packet_handler);
    } else {
        printf("[BTSTACK_HOST] Switch-BT mode: skipping HID host (device role owns PSM 0x11/0x13)\n");
    }

    // SDP server - needed for DS4/DS5 reconnection (they query Device ID)
    sdp_init();
    // Skip the adapter's own Device ID record in Switch-BT mode — switch_bt registers
    // the Nintendo (057E/2009) Device ID, and a second record would let the Switch
    // read the wrong VID/PID.
    if (!skip_hid_host) {
        device_id_create_sdp_record(device_id_sdp_service_buffer, 0x10003,
                                    DEVICE_ID_VENDOR_ID_SOURCE_BLUETOOTH,
                                    BLUETOOTH_COMPANY_ID_BLUEKITCHEN_GMBH, 1, 1);
        sdp_register_service(device_id_sdp_service_buffer);
    }
#ifdef CONFIG_BT_CLASSIC_OUTPUT
    // Switch-BT mode: register our Pro Controller HID device role now — AFTER
    // l2cap_init()/sdp_init() above (registering earlier would be wiped).
    if (skip_hid_host) switch_bt_register_device();
#endif
    printf("[BTSTACK_HOST] SDP server initialized\n");

    // Allow sniff mode and role switch for classic BT (improves compatibility)
    gap_set_default_link_policy_settings(LM_LINK_POLICY_ENABLE_SNIFF_MODE | LM_LINK_POLICY_ENABLE_ROLE_SWITCH);

    // Register for HCI events
    printf("[BTSTACK_HOST] Register event handlers...\n");
    hci_event_callback_registration.callback = packet_handler;
    hci_add_event_handler(&hci_event_callback_registration);

    // Register for SM events
    sm_event_callback_registration.callback = sm_packet_handler;
    sm_add_event_handler(&sm_event_callback_registration);

    hid_state.initialized = true;
    printf("[BTSTACK_HOST] HID handlers initialized (BLE + Classic)\n");
}

// btstack_host_init is only used for USB dongle transport
// For CYW43/ESP32, use btstack_host_init_hid_handlers() after external BTstack init
#if !defined(BTSTACK_USE_CYW43) && !defined(BTSTACK_USE_ESP32) && !defined(BTSTACK_USE_NRF)

// TLV context for flash-based link key storage (must be static/persistent)
static btstack_tlv_flash_bank_t btstack_tlv_flash_bank_context;

// Set up TLV (flash) storage for persistent link keys and BLE bonding
static void setup_tlv_storage(void) {
    printf("[BTSTACK_HOST] Setting up flash-based TLV storage...\n");

    // Check for corrupted flash banks and erase if needed
    // Flash bank 0 starts at end of flash - 8KB
    uint32_t bank0_offset = PICO_FLASH_SIZE_BYTES - (FLASH_SECTOR_SIZE * 2);
    const uint8_t* bank0_ptr = (const uint8_t*)(XIP_BASE + bank0_offset);

    // BTstack TLV expects clean flash (0xFF) or valid header
    // If we see our debug pattern (0xDEADBEEF) or other garbage, erase
    bool needs_erase = false;
    if (bank0_ptr[0] == 0xDE && bank0_ptr[1] == 0xAD &&
        bank0_ptr[2] == 0xBE && bank0_ptr[3] == 0xEF) {
        printf("[BTSTACK_HOST] Detected corrupted flash bank (debug pattern)\n");
        needs_erase = true;
    }

    if (needs_erase) {
        btstack_erase_flash_banks();
    }

    // Get the Pico SDK flash bank HAL instance
    const hal_flash_bank_t *hal_flash_bank_impl = pico_flash_bank_instance();
    printf("[BTSTACK_HOST] Flash bank instance: %p\n", hal_flash_bank_impl);

    // Initialize BTstack TLV with flash bank
    const btstack_tlv_t *btstack_tlv_impl = btstack_tlv_flash_bank_init_instance(
            &btstack_tlv_flash_bank_context,
            hal_flash_bank_impl,
            NULL);
    printf("[BTSTACK_HOST] TLV instance: %p\n", btstack_tlv_impl);

    if (!btstack_tlv_impl) {
        printf("[BTSTACK_HOST] ERROR: TLV init failed!\n");
        return;
    }

    // Set global TLV instance
    btstack_tlv_set_instance(btstack_tlv_impl, &btstack_tlv_flash_bank_context);

    // Set up Classic BT link key storage using TLV
    const btstack_link_key_db_t *btstack_link_key_db = btstack_link_key_db_tlv_get_instance(
            btstack_tlv_impl, &btstack_tlv_flash_bank_context);
    printf("[BTSTACK_HOST] Link key DB instance: %p\n", btstack_link_key_db);

    if (!btstack_link_key_db) {
        printf("[BTSTACK_HOST] ERROR: Link key DB init failed!\n");
        return;
    }

    hci_set_link_key_db(btstack_link_key_db);
    printf("[BTSTACK_HOST] Classic BT link key DB configured (flash)\n");

    // Configure BLE device DB for TLV storage
    le_device_db_tlv_configure(btstack_tlv_impl, &btstack_tlv_flash_bank_context);
    printf("[BTSTACK_HOST] BLE device DB configured (flash)\n");

    // Debug: check bank state
    printf("[BTSTACK_HOST] TLV context: current_bank=%d write_offset=0x%lX\n",
           btstack_tlv_flash_bank_context.current_bank,
           (unsigned long)btstack_tlv_flash_bank_context.write_offset);
}

void btstack_host_init(const void* transport)
{
    if (hid_state.initialized) {
        printf("[BTSTACK_HOST] Already initialized\n");
        return;
    }

    if (!transport) {
        printf("[BTSTACK_HOST] ERROR: No HCI transport provided\n");
        return;
    }

    printf("[BTSTACK_HOST] Initializing BTstack...\n");

    memset(&hid_state, 0, sizeof(hid_state));
    // Initialize BLE connection handles to invalid (handle 0 is valid in BLE)
    for (int i = 0; i < MAX_BLE_CONNECTIONS; i++) {
        hid_state.connections[i].handle = HCI_CON_HANDLE_INVALID;
    }
    hid_state.hci_transport = (const hci_transport_t*)transport;

    // HCI dump disabled - too verbose (logs every ACL packet)
    // printf("[BTSTACK_HOST] Init HCI dump (for logging)...\n");
    // hci_dump_init(hci_dump_embedded_stdout_get_instance());

#ifdef CONFIG_DS5_DROP_SCREAM
    // Silent hci_dump tap: observes L2CAP_EVENT_CHANNEL_OPENED/CLOSED to learn
    // HID interrupt CIDs for direct audio sends (no BTstack modification)
    hci_dump_init(&ds5_cid_tap);
#endif

    printf("[BTSTACK_HOST] Init memory pools...\n");
    btstack_memory_init();

    printf("[BTSTACK_HOST] Init run loop...\n");
    btstack_run_loop_init(btstack_run_loop_embedded_get_instance());

    printf("[BTSTACK_HOST] Init HCI with provided transport...\n");
    hci_init(transport, NULL);

    // Set up flash-based TLV storage for persistent link keys and BLE bonds
    setup_tlv_storage();

    // Set up HID handlers
    setup_hid_handlers();
    printf("[BTSTACK_HOST] Initialized OK\n");
}
#endif

void btstack_host_init_hid_handlers(void)
{
    if (hid_state.initialized) {
        printf("[BTSTACK_HOST] HID handlers already initialized\n");
        return;
    }

    printf("[BTSTACK_HOST] Initializing HID handlers (BTstack already initialized externally)...\n");

    memset(&hid_state, 0, sizeof(hid_state));
    // Initialize BLE connection handles to invalid (handle 0 is valid in BLE)
    for (int i = 0; i < MAX_BLE_CONNECTIONS; i++) {
        hid_state.connections[i].handle = HCI_CON_HANDLE_INVALID;
    }
    // Note: hci_transport is not set here since BTstack was initialized externally

    // Set up HID handlers (BTstack core already initialized by btstack_cyw43_init or similar)
    setup_hid_handlers();

#ifdef CONFIG_DS5_DROP_SCREAM
    // Silent hci_dump tap: observes L2CAP_EVENT_CHANNEL_OPENED/CLOSED to learn
    // HID interrupt CIDs for direct audio sends (no BTstack modification).
    // NOTE: must be here — btstack_host_init() is USB-dongle-transport only.
    hci_dump_init(&ds5_cid_tap);
    printf("[BTSTACK_HOST] DS5 audio CID tap registered\n");

    if (watchdog_hw->scratch[4] == DS5_CRASH_MAGIC) {
        crash_report_pc = watchdog_hw->scratch[5];
        crash_report_lr = watchdog_hw->scratch[6];
        crash_report_pending = true;
        watchdog_hw->scratch[4] = 0;
        printf("[CRASH] Previous boot HardFault PC=0x%08lx LR=0x%08lx\n",
               (unsigned long)crash_report_pc, (unsigned long)crash_report_lr);
    }
#endif

    printf("[BTSTACK_HOST] HID handlers initialized OK\n");
}

void btstack_host_power_on(void)
{
    printf("[BTSTACK_HOST] power_on called, initialized=%d\n", hid_state.initialized);

    if (!hid_state.initialized) {
        printf("[BTSTACK_HOST] ERROR: Not initialized\n");
        return;
    }

    printf("[BTSTACK_HOST] HCI state before power_on: %d\n", hci_get_state());
    printf("[BTSTACK_HOST] Calling hci_power_control(HCI_POWER_ON)...\n");
    int err = hci_power_control(HCI_POWER_ON);
    printf("[BTSTACK_HOST] hci_power_control returned %d, state now: %d\n", err, hci_get_state());
}

// ============================================================================
// LAST CONNECTED DEVICE PERSISTENCE (via BTstack TLV)
// ============================================================================

// TLV tag 'JPLC' = Joypad Last Connected
#define TLV_TAG_LAST_CONNECTED (((uint32_t)'J' << 24) | ((uint32_t)'P' << 16) | ((uint32_t)'L' << 8) | 'C')

typedef struct {
    bd_addr_t addr;
    uint8_t addr_type;
    char name[48];
} __attribute__((packed)) last_connected_record_t;

static void btstack_host_save_last_connected(void)
{
    const btstack_tlv_t *tlv_impl = NULL;
    void *tlv_context = NULL;
    btstack_tlv_get_instance(&tlv_impl, &tlv_context);
    if (!tlv_impl) return;

    last_connected_record_t record;
    memcpy(record.addr, hid_state.last_connected_addr, 6);
    record.addr_type = (uint8_t)hid_state.last_connected_addr_type;
    strncpy(record.name, hid_state.last_connected_name, sizeof(record.name) - 1);
    record.name[sizeof(record.name) - 1] = '\0';

    tlv_impl->store_tag(tlv_context, TLV_TAG_LAST_CONNECTED,
                        (const uint8_t *)&record, sizeof(record));
}

static void btstack_host_restore_last_connected(void)
{
    if (hid_state.has_last_connected) return;  // Already have one (e.g., from same session)

    const btstack_tlv_t *tlv_impl = NULL;
    void *tlv_context = NULL;
    btstack_tlv_get_instance(&tlv_impl, &tlv_context);
    if (!tlv_impl) return;

    last_connected_record_t record;
    int len = tlv_impl->get_tag(tlv_context, TLV_TAG_LAST_CONNECTED,
                                (uint8_t *)&record, sizeof(record));
    if (len != sizeof(record)) return;

    // Validate — addr must not be all zeros
    bool valid = false;
    for (int i = 0; i < 6; i++) {
        if (record.addr[i] != 0) { valid = true; break; }
    }
    if (!valid) return;

    memcpy(hid_state.last_connected_addr, record.addr, 6);
    hid_state.last_connected_addr_type = (bd_addr_type_t)record.addr_type;
    strncpy(hid_state.last_connected_name, record.name, sizeof(hid_state.last_connected_name) - 1);
    hid_state.last_connected_name[sizeof(hid_state.last_connected_name) - 1] = '\0';
    hid_state.has_last_connected = true;
    hid_state.reconnect_attempts = 0;

    printf("[BTSTACK_HOST] Restored last connected: %02X:%02X:%02X:%02X:%02X:%02X name='%s'\n",
           record.addr[5], record.addr[4], record.addr[3], record.addr[2], record.addr[1], record.addr[0],
           hid_state.last_connected_name);
}

// ============================================================================
// SCANNING
// ============================================================================

static uint32_t scan_timeout_end = 0;  // 0 = no timeout (indefinite scan)
static bool scan_suppressed = false;   // App can suppress auto-restart (e.g. USB device connected)

// Pending BLE gamepad: when we see a gamepad appearance or HID UUID but no name in the
// ADV packet, stash the address and wait for the scan response (which typically contains
// the name). This prevents connecting to Xbox controllers as "Generic BLE Gamepad".
static struct {
    bool valid;
    bd_addr_t addr;
    bd_addr_type_t addr_type;
    uint16_t appearance;
    bool has_hid_uuid;
    uint32_t timestamp;  // For expiry
} pending_ble_gamepad;

#define BLE_RECONNECT_INTERVAL_MS 20000  // While scanning, try reconnecting to bonded device every 20s

void btstack_host_start_scan(void)
{
#ifdef CONFIG_USB2BLE
    return;  // USB2BLE is BLE peripheral only — no scanning for input devices
#endif
#ifdef BTSTACK_DEFER_SCAN
    if (!btstack_host_scan_enabled) return;
#endif
    if (scan_suppressed) {
        return;  // App suppressed scanning (e.g. BT host disabled)
    }

    if (!hid_state.powered_on) {
        printf("[BTSTACK_HOST] Not powered on yet\n");
        return;
    }

    if (hid_state.scan_active || classic_state.inquiry_active) {
        return;  // Already scanning
    }

    printf("[BTSTACK_HOST] Starting BLE scan...\n");
    gap_set_scan_params(1, SCAN_INTERVAL, SCAN_WINDOW, 0);
    gap_start_scan();
    hid_state.scan_active = true;
    hid_state.state = BLE_STATE_SCANNING;
    if (hid_state.has_last_connected && hid_state.scan_start_time == 0) {
        // First scan with a bonded device: offset start time so periodic reconnect
        // fires after ~3s instead of the full BLE_RECONNECT_INTERVAL_MS (20s)
        hid_state.scan_start_time = btstack_run_loop_get_time_ms() - BLE_RECONNECT_INTERVAL_MS + 3000;
    } else {
        hid_state.scan_start_time = btstack_run_loop_get_time_ms();
    }

#if !defined(BTSTACK_USE_ESP32) && !defined(BTSTACK_USE_NRF) && !defined(CONFIG_USB2BLE)
    // Also start classic BT inquiry (not available on ESP32-S3/nRF BLE-only)
    // Skip in USB2BLE mode — Classic inquiry interferes with BLE advertising
    // Alternate between GIAC (general) and LIAC (limited) to discover Wiimotes/Wii U Pro
    // which use Limited Discoverable mode when SYNC button is pressed
    uint32_t lap = classic_state.use_liac ? GAP_IAC_LIMITED_INQUIRY : GAP_IAC_GENERAL_INQUIRY;
    printf("[BTSTACK_HOST] Starting Classic inquiry (LAP=%s)...\n",
           classic_state.use_liac ? "LIAC" : "GIAC");
    gap_inquiry_set_lap(lap);
    gap_inquiry_start(INQUIRY_DURATION);
    classic_state.inquiry_active = true;
#endif
}

void btstack_host_stop_scan(void)
{
    // Always set state to idle to prevent scanning from restarting
    hid_state.state = BLE_STATE_IDLE;
    hid_state.scan_start_time = 0;

    if (hid_state.scan_active) {
        printf("[BTSTACK_HOST] Stopping BLE scan\n");
        gap_stop_scan();
        hid_state.scan_active = false;
    }

#if !defined(BTSTACK_USE_ESP32) && !defined(BTSTACK_USE_NRF)
    if (classic_state.inquiry_active) {
        printf("[BTSTACK_HOST] Stopping Classic inquiry\n");
        gap_inquiry_stop();
        classic_state.inquiry_active = false;
    }
#endif
}

void btstack_host_start_timed_scan(uint32_t timeout_ms)
{
    // Never start inquiry over an in-flight Classic connection setup (e.g.
    // button pressed while a controller is mid-handshake): inquiry starves
    // the LMP encryption exchange, which stalls ~30s and dies with reason
    // 0x22 (LMP response timeout) — controller never finishes connecting.
    if (classic_state.pending_valid) {
        printf("[BTSTACK_HOST] Timed scan ignored: Classic connection setup in progress\n");
        return;
    }

    scan_suppressed = false;  // Explicit scan request clears suppression
    scan_timeout_end = btstack_run_loop_get_time_ms() + timeout_ms;
    printf("[BTSTACK_HOST] Starting timed scan (%lums)\n", (unsigned long)timeout_ms);
    btstack_host_start_scan();
}

void btstack_host_suppress_scan(bool suppress)
{
    scan_suppressed = suppress;
    if (suppress && btstack_host_is_scanning()) {
        btstack_host_stop_scan();
    }
}

// Diagnostic/bench tool: drop every BLE link and hold off all reconnection
// (rapid retries, idle ticker, scanning) for a window, so radio-contention
// A/B tests can run against a genuinely BLE-quiet dongle. The periodic task
// clears the holdoff (and un-suppresses scanning) when it expires.
static uint32_t ble_drop_holdoff_until;

void btstack_host_ble_drop_all(uint32_t holdoff_ms)
{
    ble_drop_holdoff_until = btstack_run_loop_get_time_ms() + holdoff_ms;
    scan_suppressed = true;
    if (btstack_host_is_scanning()) {
        btstack_host_stop_scan();
    }
    for (int i = 0; i < MAX_BLE_CONNECTIONS; i++) {
        if (hid_state.connections[i].handle != HCI_CON_HANDLE_INVALID) {
            gap_disconnect(hid_state.connections[i].handle);
        }
    }
    printf("[BTSTACK_HOST] BLE drop: all links down, reconnect held %lums\n",
           (unsigned long)holdoff_ms);
}

// ============================================================================
// CONNECTION
// ============================================================================

// 4s timeout for BLE connection attempts. A present device (fresh auto-connect
// off its adv, or a real bonded reconnect) completes in <2s; the old 10s only
// hurt the failure case — a stale/rotated-RPA bond held the radio with scanning
// OFF for 10s at a time, starving discovery of the device's fresh advertisement
// (the "can't pair after a bad bond" trap). Fail fast, get back to scanning.
#define BLE_CONNECT_TIMEOUT_MS 4000

void btstack_host_connect_ble(bd_addr_t addr, bd_addr_type_t addr_type)
{
    printf("[BTSTACK_HOST] Connecting to %02X:%02X:%02X:%02X:%02X:%02X\n",
           addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);

    // Stop scanning first
    btstack_host_stop_scan();

    // Save pending connection info
    memcpy(hid_state.pending_addr, addr, 6);
    hid_state.pending_addr_type = addr_type;
    hid_state.state = BLE_STATE_CONNECTING;
    hid_state.reconnect_attempt_time = btstack_run_loop_get_time_ms();

    // Create connection
    uint8_t status = gap_connect(addr, addr_type);
    printf("[BTSTACK_HOST] gap_connect returned status=%d\n", status);
}

// ============================================================================
// CALLBACKS
// ============================================================================

void btstack_host_register_report_callback(btstack_host_report_callback_t callback)
{
    hid_state.report_callback = callback;
}

void btstack_host_register_connect_callback(btstack_host_connect_callback_t callback)
{
    hid_state.connect_callback = callback;
}

// ============================================================================
// MAIN LOOP
// ============================================================================


// Transport-specific process function (weak, overridden by transport)
__attribute__((weak)) void btstack_host_transport_process(void) {
    // Default: no-op, transport should override
}

void btstack_host_process(void)
{
    if (!hid_state.initialized) return;

    // Process transport-specific tasks (e.g., USB polling, CYW43 async context)
    btstack_host_transport_process();

#ifdef CONFIG_DS5_DROP_SCREAM
    // Re-announce last crash after log listeners have had time to reattach
    if (crash_report_pending &&
        btstack_run_loop_get_time_ms() > 6000) {
        crash_report_pending = false;
        printf("[CRASH] !!! Previous boot HardFault PC=0x%08lx LR=0x%08lx — addr2line these !!!\n",
               (unsigned long)crash_report_pc, (unsigned long)crash_report_lr);
    }
#endif


#if !defined(BTSTACK_USE_CYW43) && !defined(BTSTACK_USE_ESP32) && !defined(BTSTACK_USE_NRF)
    // Process BTstack run loop multiple times to let packets flow through HCI->L2CAP->ATT->GATT
    // Note: CYW43 uses async_context, ESP32 uses FreeRTOS run loop - both process automatically
    for (int i = 0; i < 5; i++) {
        btstack_run_loop_embedded_execute_once();
    }
#endif

    // Process any pending BLE HID report (deferred from BTstack callback to avoid stack overflow)
    if (ble_report_pending) {
        ble_report_pending = false;
        route_ble_hid_report(pending_ble_conn_index, pending_ble_report, pending_ble_report_len);
    }

    // Retry Switch 2 init if stuck (no ACK received)
    switch2_retry_init_if_needed();

    // Advance deferred post-HID setup (REPORT protocol mode -> DIS/BAS/NUS)
    mp_hid_setup_task();

    // Kick off / advance MouthPad NUS discovery once HID has settled
    mp_nus_periodic();

    // Force DIS on any BLE link stuck unidentified on the generic path (name-less SC2)
    ble_identify_stall_task();

    // Advance Valve (Steam Controller 2) feature-report keepalive
    valve_periodic();

    // Handle Switch 2 rumble/LED feedback passthrough
    switch2_handle_feedback();

    // Check scan timeout
    if (scan_timeout_end > 0 && btstack_host_is_scanning()) {
        if (btstack_run_loop_get_time_ms() >= scan_timeout_end) {
            printf("[BTSTACK_HOST] Timed scan expired\n");
            scan_timeout_end = 0;
            btstack_host_stop_scan();
        }
    }

    // BLE connection attempt timeout — gap_connect() has no built-in timeout,
    // so if the target device is powered off, we'd be stuck in CONNECTING forever.
    // Cancel after BLE_CONNECT_TIMEOUT_MS. Set state to IDLE immediately to prevent
    // this check from re-triggering on the next tick. The LE_CONNECTION_COMPLETE
    // error event from the cancel will handle retry/resume logic.
    if (hid_state.state == BLE_STATE_CONNECTING &&
        hid_state.reconnect_attempt_time != 0 &&
        (btstack_run_loop_get_time_ms() - hid_state.reconnect_attempt_time) >= BLE_CONNECT_TIMEOUT_MS) {
        printf("[BTSTACK_HOST] BLE connection attempt timed out after %dms\n", BLE_CONNECT_TIMEOUT_MS);
        gap_connect_cancel();
        hid_state.state = BLE_STATE_IDLE;
        hid_state.reconnect_attempt_time = 0;
    }

    // Timeout for "waiting for incoming reconnection" after outgoing Classic HID failure.
    // If the device doesn't reconnect within 30s, give up and resume scanning.
    if (classic_state.waiting_for_incoming_time != 0 &&
        (btstack_run_loop_get_time_ms() - classic_state.waiting_for_incoming_time) >= 30000) {
        printf("[BTSTACK_HOST] Incoming reconnection timeout, resuming scan\n");
        classic_state.waiting_for_incoming_time = 0;
    }

    // Classic connection establishment timeout.
    // If a connection doesn't reach hid_ready within CLASSIC_CONNECT_TIMEOUT_MS,
    // something went wrong (e.g., CYW43 SPI bus failure during SSP pairing,
    // incompatible device, or stuck SDP query). Clean up and try to recover.
    for (int i = 0; i < MAX_CLASSIC_CONNECTIONS; i++) {
        classic_connection_t* conn = &classic_state.connections[i];
        if (conn->active && !conn->hid_ready && conn->connect_time != 0 &&
            (btstack_run_loop_get_time_ms() - conn->connect_time) >= CLASSIC_CONNECT_TIMEOUT_MS) {
            printf("[BTSTACK_HOST] Classic connection timeout after %lums (slot %d '%s'), cleaning up\n",
                   (unsigned long)(btstack_run_loop_get_time_ms() - conn->connect_time), i, conn->name);

            // Try to disconnect (may fail if BT transport is dead)
            if (conn->hid_cid != 0 && conn->hid_cid != 0xFFFF) {
                hid_host_disconnect(conn->hid_cid);
            }

            // Clean up wiimote state if this was a direct L2CAP device
            if (wiimote_conn.active && memcmp(wiimote_conn.addr, conn->addr, 6) == 0) {
                memset(&wiimote_conn, 0, sizeof(wiimote_conn));
            }

            // Clean up connection slot
            memset(conn, 0, sizeof(*conn));
            classic_state.pending_valid = false;
            classic_state.pending_hid_connect = false;

            // Start recovery timer and try to resume scanning
            classic_state.recovery_start_time = btstack_run_loop_get_time_ms();
            btstack_host_start_scan();
            break;  // Only handle one timeout per tick
        }
    }

    // Recovery watchdog: if we cleaned up a stuck connection but BT transport
    // appears dead, force a reboot. Two guards keep this from nuking live
    // sessions (it caused mid-session "stealth reboots", crash_pc=0):
    //   - any active Classic or BLE link proves the transport works — a
    //     stalled single connection setup is not a dead radio;
    //   - a GIAC inquiry takes ~10.24s, so a 10.0s deadline rebooted before
    //     the all-clear (GAP_EVENT_INQUIRY_COMPLETE) could ever land. 20s
    //     gives the inquiry room to finish.
    if (classic_state.recovery_start_time != 0 &&
        (btstack_run_loop_get_time_ms() - classic_state.recovery_start_time) >= 20000) {
        bool any_link = btstack_classic_get_connection_count() > 0;
        for (int i = 0; !any_link && i < MAX_BLE_CONNECTIONS; i++) {
            any_link = hid_state.connections[i].handle != HCI_CON_HANDLE_INVALID;
        }
        if (any_link) {
            classic_state.recovery_start_time = 0;   // transport demonstrably alive
        } else {
            printf("[BTSTACK_HOST] No BT activity after connection timeout recovery, rebooting\n");
            platform_reboot();
        }
    }

    // BLE.DROP holdoff expiry: restore normal reconnect/scan behavior.
    if (ble_drop_holdoff_until != 0 &&
        (int32_t)(btstack_run_loop_get_time_ms() - ble_drop_holdoff_until) >= 0) {
        ble_drop_holdoff_until = 0;
        scan_suppressed = false;
        printf("[BTSTACK_HOST] BLE drop holdoff expired, reconnect resumed\n");
    }

    // Safety net: if idle with no active connections and not scanning, resume scan.
    // This catches edge cases where the state machine gets stuck (e.g., gap_connect_cancel
    // doesn't generate an error event, or a disconnect handler didn't restart scanning).
    // Skip if:
    //   - scan_suppressed (app paused scanning, e.g. USB device connected)
    //   - waiting for incoming Classic reconnection (outgoing HID failed)
    //   - Classic connection setup in progress (name request, HID connect pending)
#ifndef CONFIG_USB2BLE
    if (hid_state.powered_on &&
        !scan_suppressed &&
        hid_state.state == BLE_STATE_IDLE &&
        hid_state.reconnect_attempt_time == 0 &&
        !hid_state.scan_active &&
        classic_state.waiting_for_incoming_time == 0 &&
        !classic_state.pending_valid &&
        // Classic links only. The combined count includes BLE, so one
        // unrelated BLE HID device in range (a TV remote, a keyboard) holds
        // this gate shut forever: Classic inquiry never resumes, and since the
        // Classic auto-connect path is gated on classic_state.inquiry_active, a
        // bonded Classic pad can then never be rediscovered after it powers off.
        classic_link_count() == 0) {
        printf("[BTSTACK_HOST] Safety: idle with no Classic links, resuming scan\n");
        btstack_host_start_scan();
    }
#endif

    // State/scan_active sync: if BLE scan is running but state is not SCANNING,
    // fix the desync so the advertising handler can auto-connect to devices.
    if (hid_state.scan_active && hid_state.state == BLE_STATE_IDLE) {
        printf("[BTSTACK_HOST] Safety: scan active but state IDLE, fixing to SCANNING\n");
        hid_state.state = BLE_STATE_SCANNING;
    }

    // Periodic reconnection to bonded device while scanning.
    // Many BLE devices (e.g. Stadia) don't advertise in discoverable mode after
    // bonding — they expect the central to connect directly via gap_connect().
    // After the rapid reconnect attempts (right after disconnect) are exhausted,
    // alternate between scanning and reconnection attempts.
    if (hid_state.state == BLE_STATE_SCANNING &&
        hid_state.has_last_connected &&
        hid_state.scan_start_time != 0 &&
        (btstack_run_loop_get_time_ms() - hid_state.scan_start_time) >= BLE_RECONNECT_INTERVAL_MS) {
        printf("[BTSTACK_HOST] Periodic reconnection to bonded device '%s'\n",
               hid_state.last_connected_name);
        strncpy(hid_state.pending_name, hid_state.last_connected_name, sizeof(hid_state.pending_name) - 1);
        hid_state.pending_name[sizeof(hid_state.pending_name) - 1] = '\0';
        btstack_host_connect_ble(hid_state.last_connected_addr, hid_state.last_connected_addr_type);
    }

    // Bonded-device reconnect while IDLE. The scan-gated path above never runs
    // once scanning stops, and every scan-resume path is gated on zero Classic
    // connections — so with a DualSense up, a dropped bonded BLE device could
    // never re-pair. A direct gap_connect needs no scan (and is kinder to
    // Classic coexistence than scanning), so keep dialing the bonded device
    // whenever its link is down.
    static uint32_t idle_reconnect_ms;
    if (hid_state.state == BLE_STATE_IDLE &&
        hid_state.powered_on &&
        !scan_suppressed &&
        hid_state.has_last_connected &&
        hid_state.reconnect_attempt_time == 0) {
        bool bonded_up = false;
        for (int i = 0; i < MAX_BLE_CONNECTIONS; i++) {
            if (hid_state.connections[i].handle != HCI_CON_HANDLE_INVALID &&
                memcmp(hid_state.connections[i].addr, hid_state.last_connected_addr, 6) == 0) {
                bonded_up = true;
                break;
            }
        }
        uint32_t now = btstack_run_loop_get_time_ms();
        if (!bonded_up && (now - idle_reconnect_ms) >= BLE_RECONNECT_INTERVAL_MS) {
            idle_reconnect_ms = now;
            printf("[BTSTACK_HOST] Idle reconnection to bonded device '%s'\n",
                   hid_state.last_connected_name);
            strncpy(hid_state.pending_name, hid_state.last_connected_name, sizeof(hid_state.pending_name) - 1);
            hid_state.pending_name[sizeof(hid_state.pending_name) - 1] = '\0';
            btstack_host_connect_ble(hid_state.last_connected_addr, hid_state.last_connected_addr_type);
        }
    }
}

// ============================================================================
// SDP QUERY CALLBACK (for VID/PID detection)
// ============================================================================

static void sdp_query_vid_pid_callback(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    UNUSED(channel);
    UNUSED(size);

    if (packet_type != HCI_EVENT_PACKET) return;

    uint8_t event_type = hci_event_packet_get_type(packet);

    // Debug: log connection-related HCI events for Wiimote troubleshooting
    if (wiimote_conn.active && event_type >= 0x01 && event_type <= 0x20) {
        printf("[BTSTACK_HOST] HCI event: 0x%02X\n", event_type);
    }

    switch (event_type) {
        case SDP_EVENT_QUERY_ATTRIBUTE_VALUE: {
            uint16_t attr_len = sdp_event_query_attribute_byte_get_attribute_length(packet);
            if (attr_len <= sdp_attribute_value_buffer_size) {
                uint16_t offset = sdp_event_query_attribute_byte_get_data_offset(packet);
                sdp_attribute_value[offset] = sdp_event_query_attribute_byte_get_data(packet);

                // Check if we got all bytes for this attribute
                if (offset + 1 == attr_len) {
                    uint16_t attr_id = sdp_event_query_attribute_byte_get_attribute_id(packet);
                    uint16_t value;
                    if (de_element_get_uint16(sdp_attribute_value, &value)) {
                        if (attr_id == BLUETOOTH_ATTRIBUTE_VENDOR_ID) {
                            classic_state.pending_vid = value;
                            printf("[BTSTACK_HOST] SDP VID: 0x%04X\n", value);
                        } else if (attr_id == BLUETOOTH_ATTRIBUTE_PRODUCT_ID) {
                            classic_state.pending_pid = value;
                            printf("[BTSTACK_HOST] SDP PID: 0x%04X\n", value);
                        }
                    }
                }
            }
            break;
        }
        case SDP_EVENT_QUERY_COMPLETE:
            printf("[BTSTACK_HOST] SDP query complete: VID=0x%04X PID=0x%04X\n",
                   classic_state.pending_vid, classic_state.pending_pid);

            // Update the connection struct with VID/PID
            if (classic_state.pending_vid || classic_state.pending_pid) {
                for (int i = 0; i < MAX_CLASSIC_CONNECTIONS; i++) {
                    classic_connection_t* conn = &classic_state.connections[i];
                    if (conn->active && memcmp(conn->addr, classic_state.pending_addr, 6) == 0) {
                        conn->vendor_id = classic_state.pending_vid;
                        conn->product_id = classic_state.pending_pid;
                        printf("[BTSTACK_HOST] Updated conn[%d] VID/PID: 0x%04X/0x%04X\n",
                               i, conn->vendor_id, conn->product_id);

                        // Notify bthid to re-evaluate driver selection with new VID/PID
                        bthid_update_device_info(i, conn->name,
                                                  classic_state.pending_vid,
                                                  classic_state.pending_pid);

                        // Re-send HID descriptor in case driver was re-evaluated to generic
                        // (descriptor was delivered earlier but ignored by the previous driver)
                        const uint8_t* hid_desc = hid_descriptor_storage_get_descriptor_data(conn->hid_cid);
                        uint16_t hid_desc_len = hid_descriptor_storage_get_descriptor_len(conn->hid_cid);
                        if (hid_desc && hid_desc_len > 0) {
                            bthid_set_hid_descriptor(i, hid_desc, hid_desc_len);
                        }
                        break;
                    }
                }

                // Also update wiimote_conn if active and address matches
                if (wiimote_conn.active && memcmp(wiimote_conn.addr, classic_state.pending_addr, 6) == 0) {
                    wiimote_conn.vendor_id = classic_state.pending_vid;
                    wiimote_conn.product_id = classic_state.pending_pid;
                    printf("[BTSTACK_HOST] Updated wiimote VID/PID: 0x%04X/0x%04X\n",
                           wiimote_conn.vendor_id, wiimote_conn.product_id);
                }
            }
            break;
    }
}

// ============================================================================
// HCI EVENT HANDLER
// ============================================================================

static void packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size)
{
    UNUSED(channel);
    UNUSED(size);

    if (packet_type != HCI_EVENT_PACKET) return;

    uint8_t event_type = hci_event_packet_get_type(packet);

    // Debug: log key HCI events to debug Wiimote reconnection
    // 0x04=CONNECTION_COMPLETE, 0x05=DISCONNECTION_COMPLETE, 0x06=AUTH_COMPLETE
    // 0x08=ENCRYPTION_CHANGE, 0x17=LINK_KEY_REQUEST, 0x18=LINK_KEY_NOTIFICATION
    // 0x16=PIN_CODE_REQUEST, 0x04=CONNECTION_REQUEST (offset differs)
    if (event_type == 0x17 || event_type == 0x18 || event_type == 0x06 ||
        event_type == 0x08 || event_type == 0x16) {
        printf("[BTSTACK_HOST] >>> HCI Event 0x%02X (size=%d)\n", event_type, size);
    }

    // Debug: catch GATT notifications at the global level
    if (event_type == GATT_EVENT_NOTIFICATION) {
        printf("[BTSTACK_HOST] >>> RAW GATT NOTIFICATION! len=%d\n", size);
    }

    switch (event_type) {
        case BTSTACK_EVENT_STATE:
            if (btstack_event_state_get_state(packet) == HCI_STATE_WORKING) {
                printf("[BTSTACK_HOST] HCI working\n");
                hid_state.powered_on = true;

                // Reset scan state (in case of reconnect)
                hid_state.scan_active = false;
                classic_state.inquiry_active = false;

#if !defined(BTSTACK_USE_ESP32) && !defined(BTSTACK_USE_NRF)
                // Set master role policy for incoming Classic connections
                // Wiimotes (including Wii U Pro) REQUIRE us to be master
                hci_set_master_slave_policy(0);  // 0 = always try to become master
                printf("[BTSTACK_HOST] Set master role policy\n");
#endif

                // Print our local BD_ADDR
                bd_addr_t local_addr;
                gap_local_bd_addr(local_addr);
                printf("[BTSTACK_HOST] Local BD_ADDR: %02X:%02X:%02X:%02X:%02X:%02X\n",
                       local_addr[0], local_addr[1], local_addr[2],
                       local_addr[3], local_addr[4], local_addr[5]);

                // Print chip info (see hci_transport_h2_tinyusb.h for dongle compatibility guide)
                uint16_t manufacturer = hci_get_manufacturer();
                printf("[BTSTACK_HOST] Chip Manufacturer: 0x%04X", manufacturer);
                switch (manufacturer) {
                    case 0x000A: printf(" (CSR) - OK\n"); break;
                    case 0x000D: printf(" (TI)\n"); break;
                    case 0x000F: printf(" (Broadcom) - OK\n"); break;
                    case 0x001D: printf(" (Qualcomm)\n"); break;
                    case 0x0046: printf(" (MediaTek)\n"); break;
                    case 0x005D: printf(" (Realtek) - NEEDS FIRMWARE!\n"); break;
                    case 0x0002: printf(" (Intel)\n"); break;
                    default: printf("\n"); break;
                }

                // Set local name (for devices that want to see us)
                // Skip when acting as BLE peripheral — ble_output sets its own name
#ifndef CONFIG_USB2BLE
#ifdef CONFIG_BT_CLASSIC_OUTPUT
                // In Switch-BT output mode we ARE a Pro Controller — this handler
                // runs after switch_bt_late_init and would otherwise clobber the
                // identity, so set the full Pro Controller GAP identity (name +
                // gamepad CoD + EIR advertising the HID service) here, the
                // authoritative post-HCI point. Returns before the generic setup.
                if (ble_output_get_mode() == BLE_MODE_SWITCH_BT)
                    switch_bt_apply_gap_identity();
                else
#endif
                gap_set_local_name("Joypad Adapter");
#endif

                // Enable bonding (needed for both Classic and BLE)
                gap_set_bondable_mode(1);
                // Set IO capability for "just works" pairing (no PIN required)
                gap_ssp_set_io_capability(SSP_IO_CAPABILITY_NO_INPUT_NO_OUTPUT);

#if !defined(BTSTACK_USE_ESP32) && !defined(BTSTACK_USE_NRF)
                // Classic BT setup (not available on ESP32-S3 BLE-only)
#ifndef CONFIG_USB2BLE
                // Set class of device to Computer (Desktop Workstation)
                // Skip when acting as BLE peripheral — appearance is set in adv data
#ifdef CONFIG_BT_CLASSIC_OUTPUT
                if (ble_output_get_mode() != BLE_MODE_SWITCH_BT)  // gamepad CoD already set above
#endif
                gap_set_class_of_device(0x000104);  // Major: Computer, Minor: Desktop

                // Enable SSP (Secure Simple Pairing) on the controller
                extern const hci_cmd_t hci_write_simple_pairing_mode;
                hci_send_cmd(&hci_write_simple_pairing_mode, 1);

                // Request bonding during SSP (required for BTstack to store link keys!)
                gap_ssp_set_authentication_requirement(SSP_IO_AUTHREQ_MITM_PROTECTION_NOT_REQUIRED_DEDICATED_BONDING);
                // Auto-accept incoming SSP pairing requests
                gap_ssp_set_auto_accept(1);

                // Make host discoverable and connectable for incoming connections
                // Required for Sony controllers (DS3, DS4, DS5) which initiate connections
                gap_discoverable_control(1);
                gap_connectable_control(1);
#endif
                // USB2BLE: Classic BT stays non-discoverable/non-connectable by default
#endif

#ifndef CONFIG_USB2BLE
                // Restore last connected device from NVS (for reconnection after reboot)
                btstack_host_restore_last_connected();

                // Always start scanning (discovers new devices + triggers periodic reconnect)
                btstack_host_start_scan();
#endif
            }
            break;

        case GAP_EVENT_ADVERTISING_REPORT: {
            classic_state.recovery_start_time = 0;  // radio demonstrably alive
            bd_addr_t addr;
            gap_event_advertising_report_get_address(packet, addr);
            bd_addr_type_t addr_type = gap_event_advertising_report_get_address_type(packet);
            int8_t rssi = gap_event_advertising_report_get_rssi(packet);
            uint8_t adv_len = gap_event_advertising_report_get_data_length(packet);
            const uint8_t *adv_data = gap_event_advertising_report_get_data(packet);
            uint8_t adv_event_type = gap_event_advertising_report_get_advertising_event_type(packet);

            // Parse name, appearance, and manufacturer data from advertising data
            char name[48] = {0};
            uint16_t mfr_company_id = 0;
            uint16_t sw2_vid = 0;
            uint16_t sw2_pid = 0;
            uint16_t appearance = 0;
            bool has_hid_uuid = false;

            ad_context_t context;
            for (ad_iterator_init(&context, adv_len, adv_data); ad_iterator_has_more(&context); ad_iterator_next(&context)) {
                uint8_t type = ad_iterator_get_data_type(&context);
                uint8_t len = ad_iterator_get_data_len(&context);
                const uint8_t *data = ad_iterator_get_data(&context);

                if (type == BLUETOOTH_DATA_TYPE_COMPLETE_LOCAL_NAME ||
                    type == BLUETOOTH_DATA_TYPE_SHORTENED_LOCAL_NAME) {
                    uint8_t copy_len = (len < sizeof(name) - 1) ? len : sizeof(name) - 1;
                    memcpy(name, data, copy_len);
                    name[copy_len] = 0;
                }

                // Parse GAP Appearance (2 bytes, little-endian)
                if (type == BLUETOOTH_DATA_TYPE_APPEARANCE && len >= 2) {
                    appearance = data[0] | (data[1] << 8);
                }

                // Check for HID service UUID (0x1812) in service class lists
                if ((type == BLUETOOTH_DATA_TYPE_COMPLETE_LIST_OF_16_BIT_SERVICE_CLASS_UUIDS ||
                     type == BLUETOOTH_DATA_TYPE_INCOMPLETE_LIST_OF_16_BIT_SERVICE_CLASS_UUIDS) && len >= 2) {
                    for (int i = 0; i + 1 < len; i += 2) {
                        uint16_t uuid16 = data[i] | (data[i + 1] << 8);
                        if (uuid16 == 0x1812) {  // HID Service
                            has_hid_uuid = true;
                            break;
                        }
                    }
                }

                // Check for Switch 2 controller via manufacturer data
                // Company ID 0x0553 (Nintendo for Switch 2)
                // BlueRetro uses data[1] for company ID, data[6] for VID - their data includes length byte
                // BTstack iterator strips length+type, so we use data[0] for company ID, data[5] for VID
                if (type == BLUETOOTH_DATA_TYPE_MANUFACTURER_SPECIFIC_DATA && len >= 2) {
                    mfr_company_id = data[0] | (data[1] << 8);
                    if (mfr_company_id == 0x0553) {
                        // Debug: print raw manufacturer data
                        printf("[SW2_BLE] Mfr data (%d bytes):", len);
                        for (int i = 0; i < len && i < 12; i++) {
                            printf(" %02X", data[i]);
                        }
                        printf("\n");
                        if (len >= 9) {
                            // VID at bytes 5-6, PID at bytes 7-8 (relative to after company ID)
                            // This matches BlueRetro's offsets accounting for length byte difference
                            sw2_vid = data[5] | (data[6] << 8);
                            sw2_pid = data[7] | (data[8] << 8);
                        }
                        printf("[BTSTACK_HOST] Switch 2 controller detected! VID=0x%04X PID=0x%04X\n",
                               sw2_vid, sw2_pid);
                    }
                }
            }

            // Log all BLE advertisements with names for debugging
            if (name[0] != 0) {
                #ifdef CONFIG_DS5_COMPANION
            extern bool ds5_companion_audio_active(void);
            if (!ds5_companion_audio_active())
#endif
            printf("[BTSTACK_HOST] BLE adv: %02X:%02X:%02X:%02X:%02X:%02X name=\"%s\"\n",
                       addr[5], addr[4], addr[3], addr[2], addr[1], addr[0], name);
            }

            // Merge with pending gamepad data: if we previously saw a gamepad appearance
            // or HID UUID for this address but no name (ADV packet), and now we have
            // the name (SCAN_RSP), use the merged data to identify the device properly.
            if (pending_ble_gamepad.valid &&
                memcmp(addr, pending_ble_gamepad.addr, 6) == 0) {
                // Carry appearance and HID UUID from the ADV packet if not in this packet
                if (appearance == 0) {
                    appearance = pending_ble_gamepad.appearance;
                }
                if (!has_hid_uuid) {
                    has_hid_uuid = pending_ble_gamepad.has_hid_uuid;
                }
                if (name[0]) {
                    // Got a name for the pending gamepad — clear pending and proceed
                    pending_ble_gamepad.valid = false;
                }
            }

            // Identify device by name and/or manufacturer company ID
            const bt_device_profile_t* profile = bt_device_lookup(name, mfr_company_id);
            bool is_known_controller = (profile != &BT_PROFILE_DEFAULT);

            // Generic BLE HID detection (fallback for unknown controllers)
            // Only triggers when no specific driver matched by name/manufacturer.
            // Primary signal: HID service UUID (0x1812) in advertisement
            // Fallback: GAP Appearance 0x03C3 (Joystick) or 0x03C4 (Gamepad)
            // Excludes controllers that use classic BT (DS4, DS3, DS5) — they advertise
            // BLE but must connect via classic for proper driver support.
            bool is_generic_ble_hid = false;
            if (!is_known_controller && !profile->classic_only &&
                (has_hid_uuid || appearance == 0x03C3 || appearance == 0x03C4)) {
                // If no name yet and this isn't already a scan response, defer connection
                // to wait for the scan response which typically contains the device name.
                // This prevents connecting to Xbox controllers as "Generic BLE HID".
                // Only defer once per address — if we already deferred and the scan response
                // didn't bring a name (or never arrived), connect as generic on the next ADV.
                if (!name[0] && adv_event_type != 0x04) {
                    if (!pending_ble_gamepad.valid ||
                        memcmp(addr, pending_ble_gamepad.addr, 6) != 0) {
                        pending_ble_gamepad.valid = true;
                        memcpy(pending_ble_gamepad.addr, addr, 6);
                        pending_ble_gamepad.addr_type = addr_type;
                        pending_ble_gamepad.appearance = appearance;
                        pending_ble_gamepad.has_hid_uuid = has_hid_uuid;
                        pending_ble_gamepad.timestamp = btstack_run_loop_get_time_ms();
                        printf("[BTSTACK_HOST] BLE HID (appearance=0x%04X hid_uuid=%d) with no name, waiting for scan response...\n",
                               appearance, has_hid_uuid);
                        break;
                    }
                    // Second ADV with no name for same address — proceed as generic
                    pending_ble_gamepad.valid = false;
                }
                is_generic_ble_hid = true;
                printf("[BTSTACK_HOST] Generic BLE HID detected: \"%s\" appearance=0x%04X hid_uuid=%d\n",
                       name, appearance, has_hid_uuid);
            }

            bool is_controller = is_known_controller || is_generic_ble_hid;

#ifdef CONFIG_DS5_DROP_SCREAM
            // Content build is Classic-only (DualSense): never court generic
            // BLE HID gadgets — each doomed connect attempt monopolizes the
            // radio ~10s and takes page scan down with it, blocking the DS5's
            // incoming reconnects (controller blinks then gives up).
            // Exception: JoypadOS peers (the untethered face) — the companion
            // relays FACE.* to them over NUS, and they pair fast (no doom).
            if (is_generic_ble_hid && !is_known_controller &&
                strstr(name, "JoypadOS") == NULL) {
                break;
            }
#endif

            // Auto-connect to supported BLE controllers (skip classic-only devices)
            if (hid_state.state == BLE_STATE_SCANNING && is_controller &&
                (profile->ble != BT_BLE_NONE || is_generic_ble_hid)) {
                printf("[BTSTACK_HOST] BLE controller: %02X:%02X:%02X:%02X:%02X:%02X name=\"%s\"\n",
                       addr[5], addr[4], addr[3], addr[2], addr[1], addr[0], name);
                // Determine display name from profile and PID
                const char* type_str;
                if (profile == &BT_PROFILE_SWITCH2) {
                    switch (sw2_pid) {
                        case 0x2066: type_str = "Switch 2 Joy-Con L"; break;
                        case 0x2067: type_str = "Switch 2 Joy-Con R"; break;
                        case 0x2069: type_str = "Switch 2 Pro"; break;
                        case 0x2073: type_str = "Switch 2 GameCube"; break;
                        default:     type_str = "Switch 2 Controller"; break;
                    }
                } else if (!is_generic_ble_hid) {
                    type_str = profile->name;
                } else if (appearance == 0x03C3 || appearance == 0x03C4) {
                    type_str = "Generic BLE Gamepad";
                } else {
                    type_str = "BLE HID Device";
                }
                printf("[BTSTACK_HOST] Connecting to %s...\n", type_str);
                // Use advertised name if available, otherwise use device type as fallback
                if (name[0]) {
                    strncpy(hid_state.pending_name, name, sizeof(hid_state.pending_name) - 1);
                } else {
                    strncpy(hid_state.pending_name, type_str, sizeof(hid_state.pending_name) - 1);
                }
                hid_state.pending_name[sizeof(hid_state.pending_name) - 1] = '\0';
                hid_state.pending_profile = profile;
                hid_state.pending_vid = sw2_vid;
                hid_state.pending_pid = sw2_pid;
                btstack_host_connect_ble(addr, addr_type);
            }
            break;
        }

        // Classic BT inquiry result
        case GAP_EVENT_INQUIRY_RESULT: {
            bd_addr_t addr;
            gap_event_inquiry_result_get_bd_addr(packet, addr);
            uint32_t cod = gap_event_inquiry_result_get_class_of_device(packet);

            // Parse name from extended inquiry response if available
            char name[240] = {0};
            if (gap_event_inquiry_result_get_name_available(packet)) {
                int name_len = gap_event_inquiry_result_get_name_len(packet);
                if (name_len > 0 && name_len < (int)sizeof(name)) {
                    memcpy(name, gap_event_inquiry_result_get_name(packet), name_len);
                    name[name_len] = 0;
                }
            }

            // Class of Device: Major=0x05 (Peripheral), Minor bits indicate type
            uint8_t major_class = (cod >> 8) & 0x1F;
            uint8_t minor_class = (cod >> 2) & 0x3F;
            bool is_gamepad = (major_class == 0x05) && ((minor_class & 0x0F) == 0x02);  // Gamepad
            bool is_joystick = (major_class == 0x05) && ((minor_class & 0x0F) == 0x01); // Joystick

            // Identify device by name
            const bt_device_profile_t* profile = bt_device_lookup_by_name(name);
            bool is_wiimote_family = (profile->classic == BT_CLASSIC_DIRECT_L2CAP);

            // Log all inquiry results for debugging (gamepads highlighted)
            const char* type_str = "";
            if (is_wiimote_family) type_str = " [WIIMOTE]";
            else if (is_gamepad || is_joystick) type_str = " [GAMEPAD]";
            printf("[BTSTACK_HOST] Inquiry: %02X:%02X:%02X:%02X:%02X:%02X COD=0x%06X%s %s\n",
                   addr[5], addr[4], addr[3], addr[2], addr[1], addr[0],
                   (unsigned)cod, type_str, name);

            // Auto-connect to gamepads and Wiimotes
            if ((is_gamepad || is_joystick || is_wiimote_family) && classic_state.inquiry_active) {
                // Skip if we already have an active incoming connection to this device
                // (the device connected to us before we found it in inquiry)
                if (classic_state.pending_valid && !classic_state.pending_outgoing &&
                    memcmp(classic_state.pending_addr, addr, 6) == 0) {
                    printf("[BTSTACK_HOST] Already have incoming connection from this device, skipping outgoing\n");
                    break;
                }

                printf("[BTSTACK_HOST] Classic gamepad found, connecting...\n");
                btstack_host_stop_scan();  // Stop inquiry

                // Save pending info for PIN code handler and deferred connection
                memcpy(classic_state.pending_addr, addr, 6);
                classic_state.pending_cod = cod;
                strncpy(classic_state.pending_name, name, sizeof(classic_state.pending_name) - 1);
                classic_state.pending_name[sizeof(classic_state.pending_name) - 1] = '\0';
                classic_state.pending_profile = profile;
                classic_state.pending_valid = true;
                classic_state.pending_outgoing = true;  // We initiated this connection

                // If name is unavailable, request it and defer connection to
                // REMOTE_NAME_REQUEST_COMPLETE. Wiimote-family devices (Wii U Pro,
                // Wiimote) need the name to route through the correct connection
                // path (direct L2CAP vs HID Host), and their name is not always
                // included in the Extended Inquiry Response.
                if (!name[0]) {
                    printf("[BTSTACK_HOST] Name unavailable at inquiry, requesting before connect...\n");
                    classic_state.pending_hid_connect = true;
                    gap_remote_name_request(addr, 0, 0);
                    break;
                }

                bool use_direct_l2cap = (profile->classic == BT_CLASSIC_DIRECT_L2CAP);
#ifdef BTSTACK_USE_CYW43
                // CYW43: Use direct L2CAP for Sony controllers to skip SDP.
                // SDP responses from DS4/DS5 crash the CYW43 SPI bus.
                if (profile->default_vid == 0x054C) {
                    use_direct_l2cap = true;
                    printf("[BTSTACK_HOST] CYW43: forcing direct L2CAP for Sony (skip SDP)\n");
                }
#endif
                if (use_direct_l2cap) {
                    // Direct L2CAP: skip SDP, create HID channels after encryption
                    printf("[BTSTACK_HOST] %s detected, using direct L2CAP approach\n", profile->name);
                    classic_state.pending_hid_connect = true;

                    // Initialize direct L2CAP connection state
                    memset(&wiimote_conn, 0, sizeof(wiimote_conn));
                    wiimote_conn.active = true;
                    wiimote_conn.state = WIIMOTE_STATE_IDLE;
                    memcpy(wiimote_conn.addr, addr, 6);
                    strncpy(wiimote_conn.name, name, sizeof(wiimote_conn.name) - 1);
                    wiimote_conn.class_of_device[0] = cod & 0xFF;
                    wiimote_conn.class_of_device[1] = (cod >> 8) & 0xFF;
                    wiimote_conn.class_of_device[2] = (cod >> 16) & 0xFF;
                    wiimote_conn.vendor_id = profile->default_vid;
                    wiimote_conn.product_id = profile->default_pid;

                    // Allocate classic connection slot for bthid routing
                    classic_connection_t* conn = find_free_classic_connection();
                    if (conn) {
                        int conn_index = conn - classic_state.connections;
                        memset(conn, 0, sizeof(*conn));
                        conn->active = true;
                        conn->hid_cid = 0xFFFF;  // Special marker for direct L2CAP
                        memcpy(conn->addr, addr, 6);
                        strncpy(conn->name, name, sizeof(conn->name) - 1);
                        conn->class_of_device[0] = cod & 0xFF;
                        conn->class_of_device[1] = (cod >> 8) & 0xFF;
                        conn->class_of_device[2] = (cod >> 16) & 0xFF;
                        conn->profile = profile;
                        conn->connect_time = btstack_run_loop_get_time_ms();
                        wiimote_conn.conn_index = conn_index;
                        printf("[BTSTACK_HOST] %s conn_index=%d\n", profile->name, conn_index);
                    }

                    // Create ACL connection directly (gap_connect will trigger HCI connection)
                    // We'll create L2CAP channels after encryption completes
                    printf("[BTSTACK_HOST] Creating ACL connection to %s...\n", profile->name);
                    uint8_t status = gap_connect(addr, BD_ADDR_TYPE_ACL);
                    if (status != ERROR_CODE_SUCCESS && status != ERROR_CODE_COMMAND_DISALLOWED) {
                        printf("[BTSTACK_HOST] gap_connect failed: 0x%02X\n", status);
                        wiimote_conn.active = false;
                        classic_state.pending_hid_connect = false;
                    }
                } else {
                    // Non-Wiimote: use normal hid_host_connect
                    // Use profile's hid_mode to determine SDP bypass
                    hid_protocol_mode_t mode = (profile->hid_mode == BT_HID_MODE_FALLBACK)
                        ? HID_PROTOCOL_MODE_REPORT_WITH_FALLBACK_TO_BOOT
                        : HID_PROTOCOL_MODE_REPORT;
                    uint16_t hid_cid;
                    uint8_t status = hid_host_connect(addr, mode, &hid_cid);
                    if (status == ERROR_CODE_SUCCESS) {
                        printf("[BTSTACK_HOST] hid_host_connect started, cid=0x%04X\n", hid_cid);

                        // Allocate connection slot
                        classic_connection_t* conn = find_free_classic_connection();
                        if (conn) {
                            memset(conn, 0, sizeof(*conn));
                            conn->active = true;
                            conn->hid_cid = hid_cid;
                            memcpy(conn->addr, addr, 6);
                            strncpy(conn->name, name, sizeof(conn->name) - 1);
                            conn->class_of_device[0] = cod & 0xFF;
                            conn->class_of_device[1] = (cod >> 8) & 0xFF;
                            conn->class_of_device[2] = (cod >> 16) & 0xFF;
                            conn->profile = profile;
                            conn->connect_time = btstack_run_loop_get_time_ms();
                        }
                    } else {
                        printf("[BTSTACK_HOST] hid_host_connect failed: %d\n", status);
                    }
                }
            }
            break;
        }

        case GAP_EVENT_INQUIRY_COMPLETE:
            classic_state.inquiry_active = false;
            classic_state.recovery_start_time = 0;  // BT transport is working
#ifndef CONFIG_USB2BLE
            // Restart inquiry after it completes (if we're still in scan mode)
            // Toggle between GIAC and LIAC to discover all device types
            if (hid_state.state == BLE_STATE_SCANNING) {
                classic_state.use_liac = !classic_state.use_liac;
                uint32_t lap = classic_state.use_liac ? GAP_IAC_LIMITED_INQUIRY : GAP_IAC_GENERAL_INQUIRY;
                printf("[BTSTACK_HOST] Restarting inquiry (LAP=%s)...\n",
                       classic_state.use_liac ? "LIAC" : "GIAC");
                gap_inquiry_set_lap(lap);
                gap_inquiry_start(INQUIRY_DURATION);
                classic_state.inquiry_active = true;
            }
#endif
            break;

        // Classic BT incoming connection request (DS3 connects this way)
        case HCI_EVENT_CONNECTION_REQUEST: {
            bd_addr_t addr;
            hci_event_connection_request_get_bd_addr(packet, addr);
            uint32_t cod = hci_event_connection_request_get_class_of_device(packet);
            uint8_t link_type = hci_event_connection_request_get_link_type(packet);
            printf("[BTSTACK_HOST] Incoming connection: %02X:%02X:%02X:%02X:%02X:%02X COD=0x%06X link=%d\n",
                   addr[0], addr[1], addr[2], addr[3], addr[4], addr[5], (unsigned)cod, link_type);

            // Save pending connection info for use when HID connection is established
            // Note: device name is not available yet at CONNECTION_REQUEST time.
            // Wiimote detection is deferred to CONNECTION_COMPLETE or later when
            // name resolution completes. Global master role policy (set at startup)
            // already ensures we become master for all connections.
            memcpy(classic_state.pending_addr, addr, 6);
            classic_state.pending_cod = cod;
            classic_state.pending_name[0] = '\0';  // Clear, will be filled by remote name request
            classic_state.pending_vid = 0;
            classic_state.pending_pid = 0;
            classic_state.pending_valid = true;
            classic_state.pending_outgoing = false;  // Device initiated this connection
            classic_state.waiting_for_incoming_time = 0;  // Device reconnected

            // Silence the radio NOW, before the handshake starts. Bonded
            // reconnects begin the LMP auth/encryption exchange immediately
            // after the ACL — racing the old stop-scan (which waited for the
            // remote name). If inquiry is still running when encryption
            // negotiates, the exchange starves and dies ~30s later with
            // reason 0x22. Scanning resumes via the normal paths if this
            // connection fails or ends.
            if (link_type == 1 /* ACL */) {
                printf("[BTSTACK_HOST] Incoming ACL: pausing scan for handshake\n");
                btstack_host_stop_scan();
            }
            // BTstack will auto-accept with the current master_slave_policy
            break;
        }

        case HCI_EVENT_CONNECTION_COMPLETE: {
            uint8_t status = hci_event_connection_complete_get_status(packet);
            hci_con_handle_t handle = hci_event_connection_complete_get_connection_handle(packet);
            bd_addr_t addr;
            hci_event_connection_complete_get_bd_addr(packet, addr);
            printf("[BTSTACK_HOST] Connection complete: status=%d handle=0x%04X addr=%02X:%02X:%02X:%02X:%02X:%02X\n",
                   status, handle, addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);

            // Handle connection complete for both incoming and outgoing connections
            if (status == 0) {
                if (classic_state.pending_valid &&
                    bd_addr_cmp(addr, classic_state.pending_addr) == 0) {
                    uint32_t cod = classic_state.pending_cod;

                    if (classic_state.pending_outgoing) {
                        // Outgoing connection (we initiated)
                        printf("[BTSTACK_HOST] Outgoing ACL complete, COD=0x%06X\n", cod);

                        // For Wiimotes, store ACL handle and do L2CAP-specific setup
                        if (classic_state.pending_hid_connect && wiimote_conn.active) {
                            wiimote_conn.acl_handle = handle;
                            printf("[BTSTACK_HOST] Wiimote: stored ACL handle=0x%04X\n", handle);

                            // Request remote name if we don't have it from inquiry
                            if (wiimote_conn.name[0] == '\0') {
                                gap_remote_name_request(addr, 0, 0);
                            }

                            // Query VID/PID via SDP
                            sdp_client_query_uuid16(&sdp_query_vid_pid_callback, addr,
                                                    BLUETOOTH_SERVICE_CLASS_PNP_INFORMATION);
                        }

                        // Request early authentication for direct L2CAP connections
                        // (Wiimote/Wii U Pro) where we manage channels ourselves and
                        // need PIN exchange before L2CAP setup.
                        // For hid_host_connect() connections: BTstack's HID Host handles
                        // authentication when creating HID L2CAP channels after SDP.
                        // Requesting auth here concurrently with SDP causes CYW43 SPI
                        // bus failures on devices with large HID descriptors (DS4 clones).
                        if (classic_state.pending_hid_connect && wiimote_conn.active) {
                            gap_request_security_level(handle, LEVEL_2);
                        }
                    } else {
                        // Incoming connection (device connected to us)
                        printf("[BTSTACK_HOST] Incoming ACL complete, COD=0x%06X\n", cod);
                        classic_state.pending_acl_handle = handle;

                        // Detect direct L2CAP device by pending profile (if available from prior inquiry).
                        // For incoming reconnections, pending_name is typically empty at
                        // this point — detection is deferred to HID_SUBEVENT_CONNECTION_OPENED
                        // or REMOTE_NAME_REQUEST_COMPLETE when the name becomes available.
                        const bt_device_profile_t* incoming_profile = classic_state.pending_profile;
                        if (!incoming_profile && classic_state.pending_name[0]) {
                            incoming_profile = bt_device_lookup_by_name(classic_state.pending_name);
                        }
                        bool is_direct_l2cap = (incoming_profile &&
                                                incoming_profile->classic == BT_CLASSIC_DIRECT_L2CAP);

                        if (is_direct_l2cap) {
                            // Wiimote/Wii U Pro reconnection - check role and link key
                            printf("[BTSTACK_HOST] %s detected (incoming reconnection)\n",
                                   incoming_profile->name);

                            // Wiimotes require master role - check and request if needed
                            hci_role_t current_role = gap_get_role(handle);
                            printf("[BTSTACK_HOST] Wiimote: role=%s\n",
                                   current_role == HCI_ROLE_MASTER ? "MASTER" :
                                   current_role == HCI_ROLE_SLAVE ? "SLAVE" : "UNKNOWN");
                            if (current_role != HCI_ROLE_MASTER) {
                                printf("[BTSTACK_HOST] Wiimote: requesting master role switch\n");
                                gap_request_role(addr, HCI_ROLE_MASTER);
                            }

                            // Check if we have a stored link key
                            link_key_t link_key;
                            link_key_type_t key_type;
                            bool have_key = gap_get_link_key_for_bd_addr(addr, link_key, &key_type);
                            printf("[BTSTACK_HOST] Wiimote: have_key=%d type=%d\n", have_key, have_key ? key_type : -1);

                            // Store info for when L2CAP events come in
                            memset(&wiimote_conn, 0, sizeof(wiimote_conn));
                            wiimote_conn.active = true;
                            wiimote_conn.state = WIIMOTE_STATE_IDLE;
                            wiimote_conn.conn_index = -1;  // Not assigned yet
                            memcpy(wiimote_conn.addr, addr, 6);
                            wiimote_conn.acl_handle = handle;
                            memcpy(wiimote_conn.class_of_device, &cod, 3);
                            if (classic_state.pending_name[0]) {
                                strncpy(wiimote_conn.name, classic_state.pending_name, sizeof(wiimote_conn.name) - 1);
                            }

                            // Request remote name for driver matching (need to distinguish Wii U Pro from Wiimote)
                            gap_remote_name_request(addr, 0, 0);

                            // For incoming connections (reconnection), let HID Host handle L2CAP
                            // Don't create outgoing L2CAP - it can conflict with HID Host
                            if (have_key) {
                                printf("[BTSTACK_HOST] Wiimote: handle=0x%04X, have key, waiting for HID Host\n", handle);
                                // Stop scanning now - we have an incoming connection
                                btstack_host_stop_scan();
                                // HID Host will receive HID_SUBEVENT_INCOMING_CONNECTION
                                // and we'll accept it there
                            } else {
                                // No key - this is a new pairing, wait for device to initiate
                                printf("[BTSTACK_HOST] Wiimote: handle=0x%04X, no key, waiting for pairing\n", handle);
                            }
                        }

                        if (!is_direct_l2cap && !bh_switch_bt_mode()) {
                            // Standard incoming connection flow (DS3, DS4, DS5, or unknown device).
                            // Skipped in Switch-BT device mode: the Switch is connecting to our
                            // HID device role, so hid_device + SM handle L2CAP/security — the
                            // host must not run remote-name/auth here.
                            // If this is actually a Wiimote reconnection where the name wasn't
                            // available yet, it will be detected later when the name resolves
                            // (see REMOTE_NAME_REQUEST_COMPLETE and HID_SUBEVENT_CONNECTION_OPENED).

                            // Request remote name for driver matching (we don't have it from inquiry)
                            gap_remote_name_request(addr, 0, 0);

                            // Don't query VID/PID via SDP here — BTstack HID Host runs its
                            // own SDP query after accepting the incoming connection, and the
                            // SDP client only handles one query at a time. Our VID/PID query
                            // would delay HID Host's descriptor query. Instead, query VID/PID
                            // at HID_SUBEVENT_CONNECTION_OPENED after HID channels are established.

                            // Request authentication only if we have a stored key (reconnection).
                            // For new pairings (no key), defer auth to after name resolution
                            // to avoid concurrent SDP+auth on CYW43 and to let device type
                            // detection (Switch vs Sony) determine the connection path.
                            link_key_t incoming_link_key;
                            link_key_type_t incoming_key_type;
                            if (gap_get_link_key_for_bd_addr(addr, incoming_link_key, &incoming_key_type)) {
                                gap_request_security_level(handle, LEVEL_2);
                            }
                        }
                    }
                }
            }
            break;
        }

        case L2CAP_EVENT_INCOMING_CONNECTION: {
            uint16_t psm = l2cap_event_incoming_connection_get_psm(packet);
            uint16_t cid = l2cap_event_incoming_connection_get_local_cid(packet);
            hci_con_handle_t handle = l2cap_event_incoming_connection_get_handle(packet);
            bd_addr_t addr;
            l2cap_event_incoming_connection_get_address(packet, addr);
            printf("[BTSTACK_HOST] L2CAP incoming: PSM=0x%04X cid=0x%04X handle=0x%04X\n", psm, cid, handle);

            // For Wiimotes during reconnection, we create outgoing L2CAP channels ourselves.
            // If the Wiimote also tries to create incoming channels, decline them at L2CAP level
            // to force the Wiimote to use our outgoing channels.
            if (wiimote_conn.active && wiimote_conn.acl_handle == handle &&
                (psm == PSM_HID_CONTROL || psm == PSM_HID_INTERRUPT)) {
                // If we're already creating outgoing channels (reconnection), decline incoming
                if (wiimote_conn.state >= WIIMOTE_STATE_W4_CONTROL_CONNECTED) {
                    printf("[BTSTACK_HOST] Wiimote: declining incoming L2CAP PSM=0x%04X (using outgoing channels)\n", psm);
                    l2cap_decline_connection(cid);
                    break;
                }
                // Fresh pairing or reconnection via HID Host - capture CID for direct L2CAP sending
                // HID Host will accept, but we need the CID to bypass hid_host_send_report
                printf("[BTSTACK_HOST] Wiimote: L2CAP incoming PSM=0x%04X cid=0x%04X (HID Host will accept)\n", psm, cid);
                if (psm == PSM_HID_CONTROL) {
                    wiimote_conn.control_cid = cid;
                    wiimote_conn.state = WIIMOTE_STATE_W4_CONTROL_CONNECTED;
                    printf("[BTSTACK_HOST] Wiimote: captured control CID=0x%04X from incoming\n", cid);
                } else {
                    wiimote_conn.interrupt_cid = cid;
                    wiimote_conn.state = WIIMOTE_STATE_W4_INTERRUPT_CONNECTED;
                    printf("[BTSTACK_HOST] Wiimote: captured interrupt CID=0x%04X from incoming\n", cid);
                }
            }
            break;
        }

        case L2CAP_EVENT_CHANNEL_OPENED: {
            uint8_t status = l2cap_event_channel_opened_get_status(packet);
            uint16_t psm = l2cap_event_channel_opened_get_psm(packet);
            uint16_t cid = l2cap_event_channel_opened_get_local_cid(packet);
            bd_addr_t l2cap_addr;
            l2cap_event_channel_opened_get_address(packet, l2cap_addr);
            printf("[BTSTACK_HOST] L2CAP opened: status=%d PSM=0x%04X cid=0x%04X addr=%s\n",
                   status, psm, cid, bd_addr_to_str(l2cap_addr));

            // Capture L2CAP CIDs for Wiimote connections (for direct L2CAP sending)
            // HID Host handles receiving, but we need direct L2CAP CIDs for sending
            // Note: bt_on_hid_ready is called from HID_SUBEVENT_CONNECTION_OPENED
            if (status == 0 && wiimote_conn.active &&
                memcmp(l2cap_addr, wiimote_conn.addr, 6) == 0) {
                if (psm == PSM_HID_CONTROL) {
                    wiimote_conn.control_cid = cid;
                    printf("[BTSTACK_HOST] Wiimote: captured control CID=0x%04X for direct sending\n", cid);
                } else if (psm == PSM_HID_INTERRUPT) {
                    wiimote_conn.interrupt_cid = cid;
                    printf("[BTSTACK_HOST] Wiimote: captured interrupt CID=0x%04X for direct sending\n", cid);
                }
            }
            break;
        }

        case HCI_EVENT_LE_META: {
#ifdef CONFIG_USB2BLE
            break;  // USB2BLE is a BLE peripheral — ble_output handles LE events
#endif
            uint8_t subevent = hci_event_le_meta_get_subevent_code(packet);

            switch (subevent) {
                case HCI_SUBEVENT_LE_CONNECTION_COMPLETE: {
                    hci_con_handle_t handle = hci_subevent_le_connection_complete_get_connection_handle(packet);
                    uint8_t status = hci_subevent_le_connection_complete_get_status(packet);

                    if (status != 0) {
                        printf("[BTSTACK_HOST] Connection failed: 0x%02X\n", status);
                        hid_state.reconnect_attempt_time = 0;

                        // If scan is already running (e.g. safety net started it after
                        // gap_connect_cancel timeout), restore scanning state so the
                        // advertising handler can auto-connect to devices
                        if (hid_state.scan_active) {
                            hid_state.state = BLE_STATE_SCANNING;
                            printf("[BTSTACK_HOST] Scan already active, resuming scan state\n");
                            break;
                        }

                        hid_state.state = BLE_STATE_IDLE;

                        // If reconnection attempt failed, try again or resume scanning
                        if (hid_state.has_last_connected && hid_state.reconnect_attempts < 5) {
                            hid_state.reconnect_attempts++;
                            printf("[BTSTACK_HOST] Retrying reconnection (attempt %d)...\n",
                                   hid_state.reconnect_attempts);
                            // Carry the stored name so conn->name is populated on
                            // reconnect — the MouthPad NUS relay arms on a name
                            // match, and an empty name leaves it stuck "scanning".
                            strncpy(hid_state.pending_name, hid_state.last_connected_name,
                                    sizeof(hid_state.pending_name) - 1);
                            hid_state.pending_name[sizeof(hid_state.pending_name) - 1] = '\0';
                            btstack_host_connect_ble(hid_state.last_connected_addr, hid_state.last_connected_addr_type);
                        } else {
                            printf("[BTSTACK_HOST] Reconnection failed after %d attempts, resuming scan\n",
                                   hid_state.reconnect_attempts);
                            btstack_host_start_scan();
                        }
                        break;
                    }

                    // Only the central role belongs to the host manager. When
                    // this device is also a BLE peripheral (controller_btusb),
                    // a host connecting to our gamepad output raises the SAME
                    // LE_CONNECTION_COMPLETE event with role=peripheral. Tracking
                    // it here would inflate the host connection/device count,
                    // tag it with the central's stale pending address
                    // (00:00:00:00:00:00), and kick off spurious central-side
                    // pairing. Leave incoming peripheral links to ble_output.
                    if (hci_subevent_le_connection_complete_get_role(packet) != 0) {
                        printf("[BTSTACK_HOST] Ignoring incoming peripheral connection (handle=0x%04X)\n",
                               handle);
                        break;
                    }

                    printf("[BTSTACK_HOST] Connected! handle=0x%04X\n", handle);

                    // The attempt is over — clear its timestamp. Leaving it
                    // stale disabled the idle bonded-reconnect ticker (its
                    // "no attempt in flight" guard) after the first
                    // successful connect between reboots.
                    hid_state.reconnect_attempt_time = 0;

                    // Find or create connection entry
                    ble_connection_t *conn = find_free_connection();
                    if (conn) {
                        memcpy(conn->addr, hid_state.pending_addr, 6);
                        conn->addr_type = hid_state.pending_addr_type;
                        conn->handle = handle;
                        conn->state = BLE_STATE_CONNECTED;
                        conn->connect_ms = btstack_run_loop_get_time_ms();
                        conn->dis_probe_started = false;
                        // Copy the name from pending connection
                        strncpy(conn->name, hid_state.pending_name, sizeof(conn->name) - 1);
                        conn->name[sizeof(conn->name) - 1] = '\0';
                        conn->profile = hid_state.pending_profile;
                        // On a bonded reconnect (especially after an adapter reboot),
                        // the connection is initiated by a direct gap_connect() that
                        // never set pending_profile from an advertisement. Recover the
                        // profile from the stored device name so custom-GATT devices
                        // (Steam Controller 2 -> BT_BLE_VALVE) take their dedicated path
                        // instead of falling through to generic HOGP — which leaves them
                        // "connected" but with no input. HOGP devices (Xbox, etc.) are
                        // unaffected: the generic path is what they use either way.
                        if ((!conn->profile || conn->profile == &BT_PROFILE_DEFAULT) &&
                            conn->name[0]) {
                            conn->profile = bt_device_lookup_by_name(conn->name);
                            printf("[BTSTACK_HOST] Recovered profile from name '%s': %s\n",
                                   conn->name, conn->profile->name);
                        }
                        conn->vid = hid_state.pending_vid;
                        conn->pid = hid_state.pending_pid;

                        printf("[BTSTACK_HOST] Connection stored: name='%s' profile=%s vid=0x%04X pid=0x%04X\n",
                               conn->name, conn->profile ? conn->profile->name : "default",
                               conn->vid, conn->pid);

                        // Route based on BLE strategy
                        if (conn->profile && conn->profile->ble == BT_BLE_CUSTOM) {
                            printf("[BTSTACK_HOST] %s: Skipping SM pairing, using direct ATT setup\n",
                                   conn->profile->name);
                            register_switch2_hid_listener(handle);
                        } else {
                            // Request pairing (SM will handle Secure Connections)
                            printf("[BTSTACK_HOST] Requesting pairing...\n");
                            sm_request_pairing(handle);
                        }
                    }

                    hid_state.state = BLE_STATE_CONNECTED;
                    break;
                }

            }
            break;
        }

        case HCI_EVENT_REMOTE_NAME_REQUEST_COMPLETE: {
            // Switch-BT device mode: don't run host driver-matching / hid_host_connect.
            // The incoming connection is the Switch pairing to our HID device role;
            // let hid_device handle the L2CAP channels.
            if (bh_switch_bt_mode()) break;
            bd_addr_t name_addr;
            hci_event_remote_name_request_complete_get_bd_addr(packet, name_addr);
            uint8_t name_status = hci_event_remote_name_request_complete_get_status(packet);

            if (name_status != 0) {
                printf("[BTSTACK_HOST] Remote name request failed: status=%d\n", name_status);

                // If we deferred a connection waiting for the name, fall back to
                // standard HID Host connect. This handles DS4, DS3, and other
                // controllers that may not respond to name requests.
                if (classic_state.pending_valid &&
                    classic_state.pending_outgoing &&
                    classic_state.pending_hid_connect &&
                    memcmp(name_addr, classic_state.pending_addr, 6) == 0) {
                    printf("[BTSTACK_HOST] Deferred connect: name failed, falling back\n");

#ifdef BTSTACK_USE_CYW43
                    // CYW43: if pending profile is Sony, use direct L2CAP to skip SDP
                    if (classic_state.pending_profile && classic_state.pending_profile->default_vid == 0x054C) {
                        printf("[BTSTACK_HOST] CYW43: forcing direct L2CAP for Sony (skip SDP)\n");
                        memset(&wiimote_conn, 0, sizeof(wiimote_conn));
                        wiimote_conn.active = true;
                        wiimote_conn.state = WIIMOTE_STATE_IDLE;
                        memcpy(wiimote_conn.addr, name_addr, 6);
                        wiimote_conn.class_of_device[0] = classic_state.pending_cod & 0xFF;
                        wiimote_conn.class_of_device[1] = (classic_state.pending_cod >> 8) & 0xFF;
                        wiimote_conn.class_of_device[2] = (classic_state.pending_cod >> 16) & 0xFF;
                        wiimote_conn.vendor_id = classic_state.pending_profile->default_vid;
                        wiimote_conn.product_id = classic_state.pending_profile->default_pid;

                        classic_connection_t* conn = find_free_classic_connection();
                        if (conn) {
                            int conn_index = conn - classic_state.connections;
                            memset(conn, 0, sizeof(*conn));
                            conn->active = true;
                            conn->hid_cid = 0xFFFF;
                            memcpy(conn->addr, name_addr, 6);
                            conn->class_of_device[0] = classic_state.pending_cod & 0xFF;
                            conn->class_of_device[1] = (classic_state.pending_cod >> 8) & 0xFF;
                            conn->class_of_device[2] = (classic_state.pending_cod >> 16) & 0xFF;
                            conn->profile = classic_state.pending_profile;
                            conn->connect_time = btstack_run_loop_get_time_ms();
                            wiimote_conn.conn_index = conn_index;
                        }

                        uint8_t status = gap_connect(name_addr, BD_ADDR_TYPE_ACL);
                        if (status != ERROR_CODE_SUCCESS && status != ERROR_CODE_COMMAND_DISALLOWED) {
                            printf("[BTSTACK_HOST] gap_connect failed: 0x%02X\n", status);
                            wiimote_conn.active = false;
                        }
                        classic_state.pending_hid_connect = false;
                        break;
                    }
#endif
                    classic_state.pending_hid_connect = false;

                    uint16_t hid_cid;
                    uint8_t status = hid_host_connect(name_addr, HID_PROTOCOL_MODE_REPORT, &hid_cid);
                    if (status == ERROR_CODE_SUCCESS) {
                        printf("[BTSTACK_HOST] hid_host_connect started, cid=0x%04X\n", hid_cid);
                        classic_connection_t* conn = find_free_classic_connection();
                        if (conn) {
                            memset(conn, 0, sizeof(*conn));
                            conn->active = true;
                            conn->hid_cid = hid_cid;
                            memcpy(conn->addr, name_addr, 6);
                            conn->class_of_device[0] = classic_state.pending_cod & 0xFF;
                            conn->class_of_device[1] = (classic_state.pending_cod >> 8) & 0xFF;
                            conn->class_of_device[2] = (classic_state.pending_cod >> 16) & 0xFF;
                            conn->connect_time = btstack_run_loop_get_time_ms();
                        }
                    } else {
                        printf("[BTSTACK_HOST] hid_host_connect failed: %d\n", status);
                    }
                }
                break;
            }

            {
                const char* name = hci_event_remote_name_request_complete_get_remote_name(packet);
                printf("[BTSTACK_HOST] Remote name: %s\n", name);

                // Store name if this is our pending incoming connection
                if (classic_state.pending_valid &&
                    memcmp(name_addr, classic_state.pending_addr, 6) == 0) {
                    strncpy(classic_state.pending_name, name, sizeof(classic_state.pending_name) - 1);
                    classic_state.pending_name[sizeof(classic_state.pending_name) - 1] = '\0';
                }

                // Also update any active connection with this address
                for (int i = 0; i < MAX_CLASSIC_CONNECTIONS; i++) {
                    classic_connection_t* conn = &classic_state.connections[i];
                    if (conn->active && memcmp(conn->addr, name_addr, 6) == 0) {
                        if (conn->name[0] == '\0') {
                            strncpy(conn->name, name, sizeof(conn->name) - 1);
                            conn->name[sizeof(conn->name) - 1] = '\0';
                            printf("[BTSTACK_HOST] Updated conn[%d] name: %s\n", i, conn->name);

                            // Update profile from name if not already set
                            if (!conn->profile || conn->profile == &BT_PROFILE_DEFAULT) {
                                conn->profile = bt_device_lookup_by_name(name);
                            }

                            if (conn->hid_ready) {
                                // Set VID/PID from profile defaults (Wiimote-family lacks PnP SDP)
                                const bt_device_profile_t* name_profile = bt_device_lookup_by_name(name);
                                if (name_profile->default_vid) {
                                    conn->vendor_id = name_profile->default_vid;
                                    uint16_t pid = bt_device_wiimote_pid_from_name(name);
                                    if (pid) {
                                        conn->product_id = pid;
                                        printf("[BTSTACK_HOST] Late %s detection, PID=0x%04X\n",
                                               name_profile->name, pid);
                                    }
                                }
                                // Notify BTHID of late name arrival — allows driver
                                // re-evaluation for devices matched as generic because
                                // name wasn't available at connection time
                                bthid_update_device_info(i, conn->name,
                                                         conn->vendor_id, conn->product_id);
                            }
                        }
                        break;
                    }
                }

                // Also update wiimote_conn if active
                if (wiimote_conn.active && memcmp(wiimote_conn.addr, name_addr, 6) == 0) {
                    if (wiimote_conn.name[0] == '\0') {
                        strncpy(wiimote_conn.name, name, sizeof(wiimote_conn.name) - 1);
                        wiimote_conn.name[sizeof(wiimote_conn.name) - 1] = '\0';
                        printf("[BTSTACK_HOST] Updated wiimote name: %s\n", wiimote_conn.name);
                    }
                }

                // Late direct-L2CAP device detection for incoming reconnections: if the name
                // resolves to a direct-L2CAP device and wiimote_conn wasn't set up at
                // CONNECTION_COMPLETE (because name was unknown), set it up now so
                // ENCRYPTION_CHANGE can create outgoing L2CAP channels.
                const bt_device_profile_t* late_profile = bt_device_lookup_by_name(name);
                bool late_direct_l2cap = (late_profile->classic == BT_CLASSIC_DIRECT_L2CAP);

#ifdef BTSTACK_USE_CYW43
                // On CYW43, Sony incoming reconnections use HID Host (not direct L2CAP).
                // The controller initiates its own L2CAP channels; we just need to stop
                // scanning and set default VID so the connection slot gets Sony VID.
                // (Direct L2CAP is only used for outgoing initial pairing to skip SDP.)
                if (late_profile->default_vid == 0x054C &&
                    classic_state.pending_valid &&
                    !classic_state.pending_outgoing &&
                    memcmp(name_addr, classic_state.pending_addr, 6) == 0) {
                    printf("[BTSTACK_HOST] Late Sony detection (incoming) - using HID Host path\n");
                    if (classic_state.pending_vid == 0) {
                        classic_state.pending_vid = late_profile->default_vid;
                    }
                    btstack_host_stop_scan();
                }
#endif
                if (!wiimote_conn.active &&
                    late_direct_l2cap &&
                    classic_state.pending_valid &&
                    !classic_state.pending_outgoing &&
                    memcmp(name_addr, classic_state.pending_addr, 6) == 0) {
                    printf("[BTSTACK_HOST] Late %s detection from name resolution (incoming reconnection)\n",
                           late_profile->name);
                    memset(&wiimote_conn, 0, sizeof(wiimote_conn));
                    wiimote_conn.active = true;
                    wiimote_conn.state = WIIMOTE_STATE_IDLE;
                    wiimote_conn.conn_index = -1;
                    memcpy(wiimote_conn.addr, name_addr, 6);
                    wiimote_conn.acl_handle = classic_state.pending_acl_handle;
                    memcpy(wiimote_conn.class_of_device, &classic_state.pending_cod, 3);
                    strncpy(wiimote_conn.name, name, sizeof(wiimote_conn.name) - 1);
                    wiimote_conn.name[sizeof(wiimote_conn.name) - 1] = '\0';
                    wiimote_conn.vendor_id = late_profile->default_vid;
                    wiimote_conn.product_id = classic_state.pending_pid ? classic_state.pending_pid : late_profile->default_pid;

                    // Stop scanning — we have an incoming connection to handle
                    btstack_host_stop_scan();

                    // Request auth if no stored key (first-time pairing).
                    // For reconnections, HCI auto-encrypts with the stored key.
                    link_key_t late_link_key;
                    link_key_type_t late_key_type;
                    if (!gap_get_link_key_for_bd_addr(name_addr, late_link_key, &late_key_type)) {
                        printf("[BTSTACK_HOST] No stored key, requesting auth for SSP pairing\n");
                        gap_request_security_level(classic_state.pending_acl_handle, LEVEL_2);
                    }
                }

                // Deferred outgoing connection: name was unavailable at inquiry time,
                // so we requested it before connecting. Now that the name has resolved,
                // connect using the appropriate path (direct L2CAP vs HID Host).
                if (classic_state.pending_valid &&
                    classic_state.pending_outgoing &&
                    classic_state.pending_hid_connect &&
                    memcmp(name_addr, classic_state.pending_addr, 6) == 0) {
                    // Update pending name and re-lookup profile
                    strncpy(classic_state.pending_name, name, sizeof(classic_state.pending_name) - 1);
                    classic_state.pending_name[sizeof(classic_state.pending_name) - 1] = '\0';
                    classic_state.pending_hid_connect = false;

                    const bt_device_profile_t* deferred_profile = bt_device_lookup_by_name(name);
                    classic_state.pending_profile = deferred_profile;

                    bool deferred_direct_l2cap = (deferred_profile->classic == BT_CLASSIC_DIRECT_L2CAP);
#ifdef BTSTACK_USE_CYW43
                    if (deferred_profile->default_vid == 0x054C) {
                        deferred_direct_l2cap = true;
                        printf("[BTSTACK_HOST] CYW43: forcing direct L2CAP for Sony (skip SDP)\n");
                    }
#endif
                    if (deferred_direct_l2cap) {
                        printf("[BTSTACK_HOST] Deferred connect: %s detected, using direct L2CAP\n",
                               deferred_profile->name);
                        classic_state.pending_hid_connect = true;

                        memset(&wiimote_conn, 0, sizeof(wiimote_conn));
                        wiimote_conn.active = true;
                        wiimote_conn.state = WIIMOTE_STATE_IDLE;
                        memcpy(wiimote_conn.addr, name_addr, 6);
                        strncpy(wiimote_conn.name, name, sizeof(wiimote_conn.name) - 1);
                        wiimote_conn.class_of_device[0] = classic_state.pending_cod & 0xFF;
                        wiimote_conn.class_of_device[1] = (classic_state.pending_cod >> 8) & 0xFF;
                        wiimote_conn.class_of_device[2] = (classic_state.pending_cod >> 16) & 0xFF;
                        wiimote_conn.vendor_id = deferred_profile->default_vid;
                        wiimote_conn.product_id = deferred_profile->default_pid;

                        classic_connection_t* conn = find_free_classic_connection();
                        if (conn) {
                            int conn_index = conn - classic_state.connections;
                            memset(conn, 0, sizeof(*conn));
                            conn->active = true;
                            conn->hid_cid = 0xFFFF;
                            memcpy(conn->addr, name_addr, 6);
                            strncpy(conn->name, name, sizeof(conn->name) - 1);
                            conn->class_of_device[0] = classic_state.pending_cod & 0xFF;
                            conn->class_of_device[1] = (classic_state.pending_cod >> 8) & 0xFF;
                            conn->class_of_device[2] = (classic_state.pending_cod >> 16) & 0xFF;
                            conn->profile = deferred_profile;
                            conn->connect_time = btstack_run_loop_get_time_ms();
                            wiimote_conn.conn_index = conn_index;
                        }

                        uint8_t status = gap_connect(name_addr, BD_ADDR_TYPE_ACL);
                        if (status != ERROR_CODE_SUCCESS && status != ERROR_CODE_COMMAND_DISALLOWED) {
                            printf("[BTSTACK_HOST] gap_connect failed: 0x%02X\n", status);
                            wiimote_conn.active = false;
                            classic_state.pending_hid_connect = false;
                        }
                    } else {
                        printf("[BTSTACK_HOST] Deferred connect: %s, using HID Host\n",
                               deferred_profile->name);
                        // Use profile's hid_mode for SDP bypass
                        hid_protocol_mode_t mode = (deferred_profile->hid_mode == BT_HID_MODE_FALLBACK)
                            ? HID_PROTOCOL_MODE_REPORT_WITH_FALLBACK_TO_BOOT
                            : HID_PROTOCOL_MODE_REPORT;
                        uint16_t hid_cid;
                        uint8_t status = hid_host_connect(name_addr, mode, &hid_cid);
                        if (status == ERROR_CODE_SUCCESS) {
                            printf("[BTSTACK_HOST] hid_host_connect started, cid=0x%04X\n", hid_cid);
                            classic_connection_t* conn = find_free_classic_connection();
                            if (conn) {
                                memset(conn, 0, sizeof(*conn));
                                conn->active = true;
                                conn->hid_cid = hid_cid;
                                memcpy(conn->addr, name_addr, 6);
                                strncpy(conn->name, name, sizeof(conn->name) - 1);
                                conn->class_of_device[0] = classic_state.pending_cod & 0xFF;
                                conn->class_of_device[1] = (classic_state.pending_cod >> 8) & 0xFF;
                                conn->class_of_device[2] = (classic_state.pending_cod >> 16) & 0xFF;
                                conn->profile = deferred_profile;
                                conn->connect_time = btstack_run_loop_get_time_ms();
                            }
                        } else {
                            printf("[BTSTACK_HOST] hid_host_connect failed: %d\n", status);
                        }
                    }
                }
            }
            break;
        }

        case HCI_EVENT_DISCONNECTION_COMPLETE: {
            hci_con_handle_t handle = hci_event_disconnection_complete_get_connection_handle(packet);
            uint8_t reason = hci_event_disconnection_complete_get_reason(packet);

            printf("[BTSTACK_HOST] Disconnected: handle=0x%04X reason=0x%02X\n", handle, reason);

            ble_connection_t *conn = find_connection_by_handle(handle);
            if (conn) {
                // Run FULL BLE cleanup whenever the handle matches a BLE
                // entry — even if setup never finished (conn_index still 0).
                // A half-open connection that dropped mid-discovery used to
                // fall into the Classic branch below and leak its entry +
                // wedged HIDS client, poisoning every reconnect after it.
                if (conn->conn_index > 0) {
                    printf("[BTSTACK_HOST] BLE disconnect: notifying bthid (conn_index=%d)\n", conn->conn_index);
                    bt_on_disconnect(conn->conn_index);
                }
                uint16_t dcid = conn->hids_cid;   // capture before the memset clears it
                memset(conn, 0, sizeof(*conn));
                conn->handle = HCI_CON_HANDLE_INVALID;

                // Clean up THIS connection's HIDS client (per-connection cid)
                if (dcid != 0) {
                    hids_client_disconnect(dcid);
                }
                if (hid_state.bas_cid != 0) {
                    battery_service_client_disconnect(hid_state.bas_cid);
                    hid_state.bas_cid = 0;
                }
                hid_state.gatt_state = GATT_IDLE;
                hid_state.gatt_handle = 0;

                // Unregister GATT notification listeners
                gatt_client_stop_listening_for_characteristic_value_updates(&xbox_hid_notification_listener);
                gatt_client_stop_listening_for_characteristic_value_updates(&switch2_hid_notification_listener);

                // Cancel any in-flight post-HID setup sequence for this device
                for (int i = 0; i < MAX_BLE_CONNECTIONS; i++) {
                    if (mp_hid_setup[i].handle == handle) {
                        mp_hid_setup[i].active = false;
                    }
                }

                // Tear down MouthPad NUS client if this was the MouthPad
                mp_nus_disconnected(handle);

                // Clean up Switch 2 state (ACK listener, init state machine)
                switch2_cleanup_on_disconnect();

                // Tear down Valve (Steam Controller 2) GATT client if this was it
                valve_disconnected(handle);

                // BLE disconnect — manage BLE state and reconnection
                hid_state.state = BLE_STATE_IDLE;

                // Try to reconnect to last connected device if we have one stored
                if (hid_state.has_last_connected && hid_state.reconnect_attempts < 5 &&
                    ble_drop_holdoff_until == 0) {
                    hid_state.reconnect_attempts++;
                    printf("[BTSTACK_HOST] Attempting BLE reconnection to stored device (attempt %d)...\n",
                           hid_state.reconnect_attempts);
                    printf("[BTSTACK_HOST] Connecting to %02X:%02X:%02X:%02X:%02X:%02X name='%s'\n",
                           hid_state.last_connected_addr[5], hid_state.last_connected_addr[4],
                           hid_state.last_connected_addr[3], hid_state.last_connected_addr[2],
                           hid_state.last_connected_addr[1], hid_state.last_connected_addr[0],
                           hid_state.last_connected_name);
                    // Copy stored name to pending so it's available when connection completes
                    strncpy(hid_state.pending_name, hid_state.last_connected_name, sizeof(hid_state.pending_name) - 1);
                    hid_state.pending_name[sizeof(hid_state.pending_name) - 1] = '\0';
                    btstack_host_connect_ble(hid_state.last_connected_addr, hid_state.last_connected_addr_type);
                } else if (btstack_classic_get_connection_count() == 0) {
                    // Resume scanning only if no devices remain
                    btstack_host_start_scan();
                }
            } else {
                // Classic BT disconnect — don't touch BLE state.
                // Classic reconnection/scanning is handled by HID_SUBEVENT_CONNECTION_CLOSED
                // or the outgoing HID failure handler. If we're waiting for an incoming
                // reconnection, don't restart scanning here.
                printf("[BTSTACK_HOST] Classic disconnect: handle=0x%04X (BLE state unchanged)\n", handle);

                // Clear pending connection state if this was the pending device.
                // Handles cases where ACL drops before HID opens (e.g., auth failure).
                if (classic_state.pending_valid) {
                    classic_state.pending_valid = false;
                    classic_state.pending_hid_connect = false;
                }

                // Fully reset direct-L2CAP (wiimote-path) state for this ACL.
                // Nothing else clears it on a normal disconnect of an
                // established session, and a stale wiimote_conn.active with a
                // RECYCLED ACL handle makes the incoming-channel guard decline
                // the next reconnection's HID channels — the controller then
                // stalls in the encryption phase and drops with reason 0x22.
                // (Affects Wiimotes and the Sony-direct-L2CAP path alike.)
                if (wiimote_conn.active && wiimote_conn.acl_handle == handle) {
                    printf("[BTSTACK_HOST] Clearing direct-L2CAP state for handle 0x%04X\n", handle);
                    memset(&wiimote_conn, 0, sizeof(wiimote_conn));
                    wiimote_conn.acl_handle = HCI_CON_HANDLE_INVALID;
                }
            }
            break;
        }

        case HCI_EVENT_LINK_KEY_REQUEST: {
            bd_addr_t req_addr;
            reverse_bytes(&packet[2], req_addr, 6);

            // Check if we have a stored link key
            link_key_t link_key;
            link_key_type_t key_type;
            bool have_key = gap_get_link_key_for_bd_addr(req_addr, link_key, &key_type);

            hci_connection_t *conn = hci_connection_for_bd_addr_and_type(req_addr, BD_ADDR_TYPE_ACL);
            printf("[BTSTACK_HOST] Link key request: %02X:%02X:%02X:%02X:%02X:%02X conn=%s have_key=%d type=%d\n",
                   req_addr[0], req_addr[1], req_addr[2], req_addr[3], req_addr[4], req_addr[5],
                   conn ? "YES" : "NO", have_key, have_key ? key_type : -1);

            // BTstack's hci.c handles this automatically - it will look up the key and respond
            // If no key is found, it sends negative reply which triggers PIN request for legacy pairing
            break;
        }

        // Legacy PIN code request - needed for Wiimote/Wii U Pro Controller
        // These devices don't support SSP and require a PIN code derived from BD_ADDR
        case HCI_EVENT_PIN_CODE_REQUEST: {
            bd_addr_t pin_addr;
            hci_event_pin_code_request_get_bd_addr(packet, pin_addr);
            printf("[BTSTACK_HOST] PIN code request: %02X:%02X:%02X:%02X:%02X:%02X\n",
                   pin_addr[0], pin_addr[1], pin_addr[2], pin_addr[3], pin_addr[4], pin_addr[5]);

            // Check if device needs BD_ADDR-based PIN (Wiimote/Wii U Pro)
            bool needs_bdaddr_pin = false;
            if (classic_state.pending_valid &&
                bd_addr_cmp(pin_addr, classic_state.pending_addr) == 0) {
                const bt_device_profile_t* pin_profile = classic_state.pending_profile;
                if (!pin_profile && classic_state.pending_name[0]) {
                    pin_profile = bt_device_lookup_by_name(classic_state.pending_name);
                }
                if (pin_profile && pin_profile->pin_type == BT_PIN_BDADDR) {
                    needs_bdaddr_pin = true;
                }
            }
            // Also check wiimote_conn state (may have been set up during inquiry)
            if (!needs_bdaddr_pin && wiimote_conn.active &&
                memcmp(pin_addr, wiimote_conn.addr, 6) == 0) {
                needs_bdaddr_pin = true;
            }

            if (needs_bdaddr_pin) {
                // Wiimote PIN: host's BD_ADDR reversed (when using SYNC button)
                // The PIN is 6 bytes, which is the BD_ADDR in reverse byte order
                bd_addr_t local_addr;
                gap_local_bd_addr(local_addr);
                uint8_t pin[6];
                pin[0] = local_addr[5];
                pin[1] = local_addr[4];
                pin[2] = local_addr[3];
                pin[3] = local_addr[2];
                pin[4] = local_addr[1];
                pin[5] = local_addr[0];
                printf("[BTSTACK_HOST] BD_ADDR PIN device detected, sending PIN (host BD_ADDR reversed)\n");
                gap_pin_code_response_binary(pin_addr, pin, 6);
            } else {
                // No BD_ADDR PIN needed - reject (SSP devices shouldn't ask for PIN)
                printf("[BTSTACK_HOST] PIN request rejected (no BD_ADDR PIN profile)\n");
                gap_pin_code_negative(pin_addr);
            }
            break;
        }

        case HCI_EVENT_LINK_KEY_NOTIFICATION: {
            bd_addr_t notif_addr;
            reverse_bytes(&packet[2], notif_addr, 6);
            link_key_t link_key;
            memcpy(link_key, &packet[8], 16);
            link_key_type_t key_type = (link_key_type_t)packet[24];

            printf("[BTSTACK_HOST] Link key notification: %02X:%02X:%02X:%02X:%02X:%02X type=%d\n",
                   notif_addr[0], notif_addr[1], notif_addr[2], notif_addr[3], notif_addr[4], notif_addr[5], key_type);

            // Explicitly store the link key (BTstack's auto-storage may not work for legacy pairing)
            gap_store_link_key_for_bd_addr(notif_addr, link_key, key_type);
            break;
        }

        case HCI_EVENT_AUTHENTICATION_COMPLETE: {
            uint8_t status = packet[2];
            hci_con_handle_t handle = little_endian_read_16(packet, 3);
            printf("[BTSTACK_HOST] Authentication complete: handle=0x%04X status=0x%02X\n", handle, status);

            // Handle PIN_OR_KEY_MISSING (0x06): controller cleared its link key
            // (e.g., put in pairing mode) but we still have a stale stored key.
            // Delete the stale key and disconnect so next attempt triggers fresh pairing.
            if (status == 0x06 && classic_state.pending_valid) {
                printf("[BTSTACK_HOST] Auth failed (key rejected), deleting stale link key\n");
                gap_drop_link_key_for_bd_addr(classic_state.pending_addr);

                // Clean up wiimote state if auth failed before L2CAP channels were created
                if (wiimote_conn.active && wiimote_conn.acl_handle == handle) {
                    memset(&wiimote_conn, 0, sizeof(wiimote_conn));
                }
                classic_state.pending_hid_connect = false;

                gap_disconnect(handle);
            }
            break;
        }

        case HCI_EVENT_ENCRYPTION_CHANGE: {
            hci_con_handle_t handle = hci_event_encryption_change_get_connection_handle(packet);
            uint8_t status = hci_event_encryption_change_get_status(packet);
            uint8_t enabled = hci_event_encryption_change_get_encryption_enabled(packet);

            printf("[BTSTACK_HOST] Encryption change: handle=0x%04X status=0x%02X enabled=%d\n",
                   handle, status, enabled);

            // For Wiimotes, create L2CAP control channel after encryption is enabled
            // This handles both initial pairing (state=IDLE) and reconnection (state=W4_CONTROL_CONNECTED)
            if (status == 0 && enabled && wiimote_conn.active &&
                wiimote_conn.acl_handle == handle &&
                (wiimote_conn.state == WIIMOTE_STATE_IDLE ||
                 wiimote_conn.state == WIIMOTE_STATE_W4_CONTROL_CONNECTED) &&
                wiimote_conn.control_cid == 0) {

                // For incoming reconnections, don't create outgoing L2CAP channels.
                // The controller will initiate its own channels via HID Host.
                // Creating outgoing channels conflicts with the incoming ones.
                if (classic_state.pending_valid && !classic_state.pending_outgoing) {
                    printf("[BTSTACK_HOST] Wiimote: incoming reconnection, waiting for HID Host channels\n");
                    break;
                }

                printf("[BTSTACK_HOST] Wiimote: encryption enabled, creating HID Control channel (PSM 0x11)...\n");

                uint16_t control_cid;
                uint8_t l2cap_status = l2cap_create_channel(wiimote_l2cap_packet_handler,
                                                            wiimote_conn.addr,
                                                            PSM_HID_CONTROL,
                                                            0xFFFF,  // MTU
                                                            &control_cid);
                if (l2cap_status == ERROR_CODE_SUCCESS) {
                    wiimote_conn.control_cid = control_cid;
                    wiimote_conn.state = WIIMOTE_STATE_W4_CONTROL_CONNECTED;
                    printf("[BTSTACK_HOST] Wiimote: L2CAP control channel request sent, cid=0x%04X\n", control_cid);
                } else {
                    printf("[BTSTACK_HOST] Wiimote: l2cap_create_channel failed: 0x%02X\n", l2cap_status);
                    wiimote_conn.active = false;
                    classic_state.pending_hid_connect = false;
                }
            }
            break;
        }

        case GAP_EVENT_SECURITY_LEVEL: {
            hci_con_handle_t handle = gap_event_security_level_get_handle(packet);
            gap_security_level_t level = gap_event_security_level_get_security_level(packet);
            printf("[BTSTACK_HOST] Security level update: handle=0x%04X level=%d\n", handle, level);
            break;
        }

        case HCI_EVENT_ROLE_CHANGE: {
            uint8_t status = hci_event_role_change_get_status(packet);
            bd_addr_t addr;
            hci_event_role_change_get_bd_addr(packet, addr);
            uint8_t role = hci_event_role_change_get_role(packet);
            printf("[BTSTACK_HOST] Role change: %02X:%02X:%02X:%02X:%02X:%02X status=%d role=%s\n",
                   addr[0], addr[1], addr[2], addr[3], addr[4], addr[5],
                   status, role == 0 ? "MASTER" : "SLAVE");
            break;
        }
    }
}

// ============================================================================
// SM EVENT HANDLER
// ============================================================================

static void sm_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size)
{
    UNUSED(channel);
    UNUSED(size);

    if (packet_type != HCI_EVENT_PACKET) return;

#ifdef CONFIG_USB2BLE
    return;  // USB2BLE is a BLE peripheral — ble_output handles SM events
#endif

    uint8_t event_type = hci_event_packet_get_type(packet);

    switch (event_type) {
        case SM_EVENT_JUST_WORKS_REQUEST:
            printf("[BTSTACK_HOST] SM: Just Works request\n");
            sm_just_works_confirm(sm_event_just_works_request_get_handle(packet));
            break;

        case SM_EVENT_PAIRING_STARTED:
            printf("[BTSTACK_HOST] SM: Pairing started\n");
            break;

        case SM_EVENT_PAIRING_COMPLETE: {
            hci_con_handle_t handle = sm_event_pairing_complete_get_handle(packet);
            uint8_t status = sm_event_pairing_complete_get_status(packet);
            printf("[BTSTACK_HOST] SM: Pairing complete, handle=0x%04X status=0x%02X\n", handle, status);

            if (status == ERROR_CODE_SUCCESS) {
                printf("[BTSTACK_HOST] SM: Pairing successful!\n");
                ble_connection_t* conn = find_connection_by_handle(handle);
                if (conn) {
                    // Store for reconnection
                    memcpy(hid_state.last_connected_addr, conn->addr, 6);
                    hid_state.last_connected_addr_type = conn->addr_type;
                    strncpy(hid_state.last_connected_name, conn->name, sizeof(hid_state.last_connected_name) - 1);
                    hid_state.last_connected_name[sizeof(hid_state.last_connected_name) - 1] = '\0';
                    hid_state.has_last_connected = true;
                    hid_state.reconnect_attempts = 0;
                    btstack_host_save_last_connected();
                    printf("[BTSTACK_HOST] Stored device for reconnection: %02X:%02X:%02X:%02X:%02X:%02X name='%s'\n",
                           conn->addr[5], conn->addr[4], conn->addr[3], conn->addr[2], conn->addr[1], conn->addr[0],
                           hid_state.last_connected_name);

                    // Recover a name-gated profile if the ADV had no name (bonded reconnect)
                    ble_resolve_profile_from_name(conn);

                    // Route based on BLE strategy
                    if (conn->profile && conn->profile->ble == BT_BLE_DIRECT_ATT) {
                        printf("[BTSTACK_HOST] %s detected - using fast-path HID listener\n",
                               conn->profile->name);
                        register_ble_hid_listener(handle);
                    } else if (conn->profile && conn->profile->ble == BT_BLE_CUSTOM) {
                        printf("[BTSTACK_HOST] %s detected - using fast-path notification enable\n",
                               conn->profile->name);
                        register_switch2_hid_listener(handle);
                    } else if (conn->profile && conn->profile->ble == BT_BLE_VALVE) {
                        printf("[BTSTACK_HOST] %s detected - starting Valve GATT discovery\n",
                               conn->profile->name);
                        register_valve_hid_listener(handle);
                    } else {
                        printf("[BTSTACK_HOST] BLE controller - starting GATT discovery\n");
                        start_hids_client(conn);
                        // MouthPad NUS is armed later, from the HID
                        // REPORTS_NOTIFICATION (0x1C) handler, so it doesn't
                        // contend with the HID notification enable.
                    }
                }
            } else {
                printf("[BTSTACK_HOST] SM: Pairing FAILED\n");
            }
            break;
        }

        case SM_EVENT_REENCRYPTION_STARTED:
            printf("[BTSTACK_HOST] SM: Re-encryption started\n");
            break;

        case SM_EVENT_REENCRYPTION_COMPLETE: {
            hci_con_handle_t handle = sm_event_reencryption_complete_get_handle(packet);
            uint8_t status = sm_event_reencryption_complete_get_status(packet);
            printf("[BTSTACK_HOST] SM: Re-encryption complete, handle=0x%04X status=0x%02X\n", handle, status);
            if (status == ERROR_CODE_SUCCESS) {
                printf("[BTSTACK_HOST] SM: Re-encryption successful!\n");
                ble_connection_t* conn = find_connection_by_handle(handle);
                if (conn) {
                    // Reset reconnect counter on successful re-encryption
                    hid_state.reconnect_attempts = 0;

                    // Update stored device info (in case address type changed or for reconnection)
                    memcpy(hid_state.last_connected_addr, conn->addr, 6);
                    hid_state.last_connected_addr_type = conn->addr_type;
                    if (conn->name[0] != '\0') {
                        strncpy(hid_state.last_connected_name, conn->name, sizeof(hid_state.last_connected_name) - 1);
                        hid_state.last_connected_name[sizeof(hid_state.last_connected_name) - 1] = '\0';
                    }
                    hid_state.has_last_connected = true;
                    btstack_host_save_last_connected();

                    // Recover a name-gated profile if the ADV had no name (bonded reconnect)
                    ble_resolve_profile_from_name(conn);

                    // Route based on BLE strategy
                    if (conn->profile && conn->profile->ble == BT_BLE_DIRECT_ATT) {
                        printf("[BTSTACK_HOST] %s detected - using fast-path HID listener\n",
                               conn->profile->name);
                        register_ble_hid_listener(handle);
                    } else if (conn->profile && conn->profile->ble == BT_BLE_CUSTOM) {
                        printf("[BTSTACK_HOST] %s detected - using fast-path notification enable\n",
                               conn->profile->name);
                        register_switch2_hid_listener(handle);
                    } else if (conn->profile && conn->profile->ble == BT_BLE_VALVE) {
                        printf("[BTSTACK_HOST] %s detected - starting Valve GATT discovery\n",
                               conn->profile->name);
                        register_valve_hid_listener(handle);
                    } else {
                        printf("[BTSTACK_HOST] BLE controller - starting GATT discovery\n");
                        start_hids_client(conn);
                        // MouthPad NUS is armed later, from the HID
                        // REPORTS_NOTIFICATION (0x1C) handler, so it doesn't
                        // contend with the HID notification enable.
                    }
                }
            } else {
                // Re-encryption failed - remote likely lost bonding info
                // Delete local bonding and request fresh pairing
                printf("[BTSTACK_HOST] SM: Re-encryption failed, deleting bond and re-pairing...\n");
                bd_addr_t addr;
                sm_event_reencryption_complete_get_address(packet, addr);
                bd_addr_type_t addr_type = sm_event_reencryption_complete_get_addr_type(packet);
                gap_delete_bonding(addr_type, addr);
                sm_request_pairing(handle);
            }
            break;
        }
    }
}

// ============================================================================
// NUS (Nordic UART Service) CLIENT
// ============================================================================
// Self-contained GATT client for a peer's NUS stream. Acts ONLY on recognized
// NUS peers (gated by mp_nus_mark_pending, which the connection-ready path
// calls for device names containing "MouthPad" or "JoypadOS", and the DIS
// path for their PnP IDs), so it has no effect on any other controller.
// Peers: Augmental MouthPad (CDC relay via mp_bridge) and JoypadOS BLE
// controllers (FACE.* command relay from cdc_commands). Discovery is dynamic by
// 128-bit UUID (no hardcoded handles) and is deferred ~1.5 s after connect so
// it runs after the HIDS client has finished its own GATT discovery (the
// gatt_client allows one query at a time per connection).
//
// Device->host NUS notifications fire mp_nus_rx_cb; host->device writes go
// through btstack_host_mouthpad_nus_send(). The CDC<->NUS framing/relay glue
// lives in mp_bridge.c.

// NUS 128-bit UUIDs (textual / big-endian order, as BTstack uuid128 expects).
static const uint8_t nus_service_uuid128[16] = {
    0x6E,0x40,0x00,0x01,0xB5,0xA3,0xF3,0x93,0xE0,0xA9,0xE5,0x0E,0x24,0xDC,0xCA,0x9E};
static const uint8_t nus_rx_uuid128[16] = {  // write  (host -> device)
    0x6E,0x40,0x00,0x02,0xB5,0xA3,0xF3,0x93,0xE0,0xA9,0xE5,0x0E,0x24,0xDC,0xCA,0x9E};
static const uint8_t nus_tx_uuid128[16] = {  // notify (device -> host)
    0x6E,0x40,0x00,0x03,0xB5,0xA3,0xF3,0x93,0xE0,0xA9,0xE5,0x0E,0x24,0xDC,0xCA,0x9E};

typedef enum {
    MP_NUS_IDLE = 0,
    MP_NUS_PENDING,        // connected, waiting for HID discovery to settle
    MP_NUS_DISC_SERVICE,
    MP_NUS_DISC_CHARS,
    MP_NUS_ENABLE_CCC,
    MP_NUS_READY,
} mp_nus_state_t;

static struct {
    mp_nus_state_t state;
    hci_con_handle_t handle;
    uint32_t pending_since;
    gatt_client_service_t service;
    gatt_client_characteristic_t tx_char;      // notify characteristic
    uint16_t rx_value_handle;                  // write characteristic value handle
    gatt_client_notification_t notify;
    uint8_t last_battery;                      // last BAS level seen (0 = unknown)
    char    firmware[24];                      // DIS firmware revision (for relay device_info)
} mp_nus = { .state = MP_NUS_IDLE, .handle = HCI_CON_HANDLE_INVALID };

// Host->device NUS write queue (drained on the BTstack run loop — see
// btstack_host_mouthpad_nus_send below). Declared here so mp_nus_reset() can
// flush it on disconnect.
#define MP_TX_SLOTS      8                  // host->device queue depth (low-rate)
#define MP_TX_SLOT_SIZE  247                // <= NUS MTU payload
typedef struct { uint16_t len; uint8_t data[MP_TX_SLOT_SIZE]; } mp_tx_slot_t;
static mp_tx_slot_t      mp_tx[MP_TX_SLOTS];
static volatile uint32_t mp_tx_head;        // write index (producer: main loop)
static volatile uint32_t mp_tx_tail;        // read index (consumer: run loop)
static volatile bool     mp_tx_scheduled;
static btstack_context_callback_registration_t mp_tx_cb;

static void (*mp_nus_rx_cb)(const uint8_t* data, uint16_t len) = NULL;

void btstack_host_set_mouthpad_nus_rx_cb(void (*cb)(const uint8_t*, uint16_t))
{
    mp_nus_rx_cb = cb;
}

bool btstack_host_mouthpad_nus_ready(void)
{
    return mp_nus.state == MP_NUS_READY;
}

// Diagnostic: NUS client state + whether the GATT client is free (0 = busy).
int btstack_host_nus_debug(int* gatt_ready)
{
    if (gatt_ready) {
        *gatt_ready = (mp_nus.handle != HCI_CON_HANDLE_INVALID)
                          ? (int)gatt_client_is_ready(mp_nus.handle) : -1;
    }
    return (int)mp_nus.state;
}

// Generic aliases: the client serves any recognized NUS peer (MouthPad or
// JoypadOS face controller), so new callers get peer-neutral names.
bool btstack_host_nus_ready(void)
{
    return btstack_host_mouthpad_nus_ready();
}

bool btstack_host_nus_send(const uint8_t* data, uint16_t len)
{
    return btstack_host_mouthpad_nus_send(data, len);
}

// Fill `out` with the connected MouthPad's device info (for the dongle-level
// relay device_info_response / connection_status_response). Returns false if no
// MouthPad connection exists.
bool btstack_host_get_mouthpad_info(btstack_host_mouthpad_info_t* out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (mp_nus.handle == HCI_CON_HANDLE_INVALID) return false;
    ble_connection_t* c = find_connection_by_handle(mp_nus.handle);
    if (!c) return false;
    strncpy(out->name, c->name, sizeof(out->name) - 1);
    strncpy(out->firmware, mp_nus.firmware, sizeof(out->firmware) - 1);
    memcpy(out->addr, c->addr, 6);
    out->vid = c->vid;
    out->pid = c->pid;
    out->battery = mp_nus.last_battery;
    out->ready = (mp_nus.state == MP_NUS_READY);
    return true;
}

// Called from the connection-ready path only for MouthPad devices.
static void mp_nus_mark_pending(hci_con_handle_t handle)
{
    if (mp_nus.state != MP_NUS_IDLE) return;   // one MouthPad NUS at a time
    mp_nus.state = MP_NUS_PENDING;
    mp_nus.handle = handle;
    mp_nus.pending_since = btstack_run_loop_get_time_ms();
    mp_nus.tx_char.value_handle = 0;
    mp_nus.rx_value_handle = 0;
    printf("[MP_NUS] MouthPad connected (0x%04X) — NUS discovery pending\n", handle);
}

static void mp_nus_reset(void)
{
    if (mp_nus.state == MP_NUS_READY) {
        gatt_client_stop_listening_for_characteristic_value_updates(&mp_nus.notify);
    }
    mp_nus.state = MP_NUS_IDLE;
    mp_nus.handle = HCI_CON_HANDLE_INVALID;
    mp_nus.tx_char.value_handle = 0;
    mp_nus.rx_value_handle = 0;
    mp_nus.last_battery = 0;
    mp_nus.firmware[0] = '\0';
    // Discard any queued host->device writes for the gone MouthPad.
    mp_tx_tail = mp_tx_head;
    mp_tx_scheduled = false;
}

static void mp_nus_disconnected(hci_con_handle_t handle)
{
    if (mp_nus.handle == handle) {
        printf("[MP_NUS] MouthPad disconnected — NUS reset\n");
        mp_nus_reset();
    }
}

// Device -> host notifications on the NUS TX characteristic.
static void mp_nus_notify_handler(uint8_t packet_type, uint16_t channel, uint8_t* packet, uint16_t size)
{
    UNUSED(channel); UNUSED(size);
    if (packet_type != HCI_EVENT_PACKET) return;
    if (hci_event_packet_get_type(packet) != GATT_EVENT_NOTIFICATION) return;
    uint16_t vh = gatt_event_notification_get_value_handle(packet);
    if (vh != mp_nus.tx_char.value_handle) return;
    uint16_t len = gatt_event_notification_get_value_length(packet);
    const uint8_t* val = gatt_event_notification_get_value(packet);
    if (mp_nus_rx_cb) mp_nus_rx_cb(val, len);
}

// GATT discovery state machine for the NUS service.
static void mp_nus_gatt_handler(uint8_t packet_type, uint16_t channel, uint8_t* packet, uint16_t size)
{
    UNUSED(channel); UNUSED(size);
    if (packet_type != HCI_EVENT_PACKET) return;
    uint8_t event = hci_event_packet_get_type(packet);

    switch (event) {
        case GATT_EVENT_SERVICE_QUERY_RESULT:
            gatt_event_service_query_result_get_service(packet, &mp_nus.service);
            break;

        case GATT_EVENT_CHARACTERISTIC_QUERY_RESULT: {
            gatt_client_characteristic_t ch;
            gatt_event_characteristic_query_result_get_characteristic(packet, &ch);
            if (memcmp(ch.uuid128, nus_tx_uuid128, 16) == 0) {
                mp_nus.tx_char = ch;
            } else if (memcmp(ch.uuid128, nus_rx_uuid128, 16) == 0) {
                mp_nus.rx_value_handle = ch.value_handle;
            }
            break;
        }

        case GATT_EVENT_QUERY_COMPLETE: {
            uint8_t status = gatt_event_query_complete_get_att_status(packet);
            if (status != 0) {
                printf("[MP_NUS] GATT query failed (state=%d status=0x%02X)\n", mp_nus.state, status);
                mp_nus_reset();
                break;
            }
            if (mp_nus.state == MP_NUS_DISC_SERVICE) {
                if (mp_nus.service.start_group_handle == 0) {
                    printf("[MP_NUS] No NUS service on device\n");
                    mp_nus_reset();
                    break;
                }
                mp_nus.state = MP_NUS_DISC_CHARS;
                gatt_client_discover_characteristics_for_service(
                    mp_nus_gatt_handler, mp_nus.handle, &mp_nus.service);
            } else if (mp_nus.state == MP_NUS_DISC_CHARS) {
                if (mp_nus.tx_char.value_handle == 0) {
                    printf("[MP_NUS] NUS TX characteristic not found\n");
                    mp_nus_reset();
                    break;
                }
                mp_nus.state = MP_NUS_ENABLE_CCC;
                gatt_client_write_client_characteristic_configuration(
                    mp_nus_gatt_handler, mp_nus.handle, &mp_nus.tx_char,
                    GATT_CLIENT_CHARACTERISTICS_CONFIGURATION_NOTIFICATION);
            } else if (mp_nus.state == MP_NUS_ENABLE_CCC) {
                gatt_client_listen_for_characteristic_value_updates(
                    &mp_nus.notify, mp_nus_notify_handler, mp_nus.handle, &mp_nus.tx_char);
                mp_nus.state = MP_NUS_READY;
                printf("[MP_NUS] NUS ready (tx=0x%04X rx=0x%04X)\n",
                       mp_nus.tx_char.value_handle, mp_nus.rx_value_handle);
            }
            break;
        }
    }
}

// NOTE: an RSSI-poll "liveness" watchdog was tried here and removed —
// HCI_Read_RSSI is answered by the LOCAL controller (not the peer), so it
// can't detect a dead link; under Classic+BLE coexistence load the CYW43
// delays the command-complete and the watchdog shot healthy links every 8s.
// Zombie links are covered by the peripheral's 6s supervision timeout plus
// the NUS re-arm / GATT-wedge watchdogs below.

// Periodic: kick off discovery once the HID side has settled.
static void mp_nus_periodic(void)
{
    if (mp_nus.state == MP_NUS_IDLE) {
        // Self-heal: a NUS peer is connected but the client is unarmed —
        // discovery failed once (e.g. raced the HIDS client right after a
        // reconnect) or the arming event was missed. Without this the FACE
        // relay stays dead while the BLE link is perfectly healthy. Re-arm
        // with a gentle backoff.
        static uint32_t next_rearm_ms = 0;
        uint32_t now = btstack_run_loop_get_time_ms();
        if (now < next_rearm_ms) return;
        next_rearm_ms = now + 3000;
        for (int i = 0; i < MAX_BLE_CONNECTIONS; i++) {
            ble_connection_t* bc = &hid_state.connections[i];
            if (bc->handle == HCI_CON_HANDLE_INVALID) continue;
            if (hci_connection_for_handle(bc->handle) == NULL)
                continue;   // stale entry — never operate on a dead handle
            if (strstr(bc->name, "MouthPad") != NULL ||
                strstr(bc->name, "JoypadOS") != NULL ||
                (bc->vid == 0x1915 && bc->pid == 0xEEEE) ||
                (bc->vid == 0x2E8A && bc->pid == 0x10C6)) {
                printf("[MP_NUS] Re-arming NUS for connected peer '%s'\n",
                       bc->name);
                mp_nus_mark_pending(bc->handle);
                break;
            }
        }
        return;
    }
    if (mp_nus.state != MP_NUS_PENDING) return;
    if ((btstack_run_loop_get_time_ms() - mp_nus.pending_since) < 1500) return;
    if (gatt_client_is_ready(mp_nus.handle) == 0) {
        // Watchdog: if the GATT client stays busy (a wedged HIDS query after
        // an ungraceful reconnect), NUS can never arm and the relay is dead
        // despite a live link. Force a clean reconnect.
        if ((btstack_run_loop_get_time_ms() - mp_nus.pending_since) > 15000) {
            // only touch the link if the HCI connection actually still
            // exists — gap_disconnect on a stale handle asserts inside
            // BTstack (crash-reboots the dongle)
            if (hci_connection_for_handle(mp_nus.handle) != NULL) {
                printf("[MP_NUS] GATT client wedged for 15s — forcing reconnect\n");
                gap_disconnect(mp_nus.handle);
            } else {
                printf("[MP_NUS] Wedged on a stale handle — resetting client\n");
            }
            mp_nus_reset();
        }
        return;   // another query in flight
    }
    mp_nus.state = MP_NUS_DISC_SERVICE;
    mp_nus.service.start_group_handle = 0;
    printf("[MP_NUS] Starting NUS discovery on 0x%04X\n", mp_nus.handle);
    gatt_client_discover_primary_services_by_uuid128(
        mp_nus_gatt_handler, mp_nus.handle, nus_service_uuid128);
}

// Deferred post-HID setup: REPORT protocol mode, then DIS/BAS + NUS arm.
static void mp_hid_setup_task(void)
{
    uint32_t now = btstack_run_loop_get_time_ms();

    // Run the REPORT-mode + notification-enable sequence for EACH connecting BLE
    // HID device independently (per-connection cid), so two devices both stream.
    for (int i = 0; i < MAX_BLE_CONNECTIONS; i++) {
        if (!mp_hid_setup[i].active) continue;
        uint16_t cid = mp_hid_setup[i].hids_cid;

        switch (mp_hid_setup[i].phase) {
            case 0: {
                // Write REPORT protocol mode FIRST. The MouthPad boots in BOOT mode
                // and BTstack's hids_client never writes the mode when REPORT is
                // requested. hids_client only accepts this once back in CONNECTED
                // state (returns 0x0C COMMAND_DISALLOWED until then), so retry.
                uint8_t st = hids_client_send_set_protocol_mode(cid, 0, HID_PROTOCOL_MODE_REPORT);
                if (st == ERROR_CODE_SUCCESS) {
                    printf("[MP] REPORT protocol-mode write initiated (cid=0x%04X)\n", cid);
                    mp_hid_setup[i].phase = 1;
                    mp_hid_setup[i].phase_ms = now;
                } else if ((now - mp_hid_setup[i].start_ms) > 3000) {
                    printf("[MP] protocol-mode write never accepted (last=0x%02X) — enabling anyway\n", st);
                    mp_hid_setup[i].phase = 1;
                    mp_hid_setup[i].phase_ms = now;
                }
                break;
            }
            case 1: {
                // After the write-without-response flushes, enable HID notifications
                // (NOW that the device is in REPORT mode, so the CCCs stick). Retry
                // until hids_client accepts it. DIS/BAS/NUS start from the 0x1C event.
                if ((now - mp_hid_setup[i].phase_ms) < 300) break;
                uint8_t r = hids_client_enable_notifications(cid);
                if (r == ERROR_CODE_SUCCESS) {
                    printf("[MP] notifications enabled after REPORT-mode switch (cid=0x%04X)\n", cid);
                    mp_hid_setup[i].active = false;
                } else if ((now - mp_hid_setup[i].start_ms) > 6000) {
                    printf("[MP] enable_notifications never accepted (last=0x%02X)\n", r);
                    mp_hid_setup[i].active = false;
                }
                break;
            }
            default:
                mp_hid_setup[i].active = false;
                break;
        }
    }
}

// Host -> device write (called from the bridge; safe in BTstack/run-loop context).
// ---------------------------------------------------------------------------
// Host->device NUS write, marshaled onto the BTstack run loop.
//
// btstack_host_mouthpad_nus_send() is called from the CDC/relay context (the
// main loop), but gatt_client_write MUST run on the BTstack thread: on nRF/ESP
// BTstack lives in its own RTOS task, and calling GATT APIs cross-thread races
// with BTstack's own processing and corrupts its state. So enqueue the payload
// into a lock-free SPSC ring and schedule a drain via
// btstack_run_loop_execute_on_main_thread() — the run-loop hop every platform
// HAL implements (nRF/ESP/CYW43) — which runs on the BTstack thread. On RP2040
// (cooperative run loop) this is the same context, just one iteration later.
// Single producer (main loop) + single consumer (run loop); both on one core,
// so volatile indices + a publish-after-copy are race-free under preemption.
// (The queue + indices are declared up by the mp_nus struct.)
// ---------------------------------------------------------------------------

// Runs on the BTstack run loop (BTstack thread) — drain the queue.
static void mp_tx_pump(void* ctx)
{
    (void)ctx;
    mp_tx_scheduled = false;                 // clear first so a late enqueue re-schedules
    while (mp_tx_head != mp_tx_tail) {
        mp_tx_slot_t* s = &mp_tx[mp_tx_tail % MP_TX_SLOTS];
        if (mp_nus.state == MP_NUS_READY && mp_nus.rx_value_handle != 0) {
            gatt_client_write_value_of_characteristic_without_response(
                mp_nus.handle, mp_nus.rx_value_handle, s->len, s->data);
        }
        mp_tx_tail++;
    }
}

bool btstack_host_mouthpad_nus_send(const uint8_t* data, uint16_t len)
{
    if (mp_nus.state != MP_NUS_READY || mp_nus.rx_value_handle == 0) return false;
    if (len == 0 || len > MP_TX_SLOT_SIZE) return false;
    if (mp_tx_head - mp_tx_tail >= MP_TX_SLOTS) return false;   // queue full

    mp_tx_slot_t* s = &mp_tx[mp_tx_head % MP_TX_SLOTS];
    memcpy(s->data, data, len);
    s->len = len;
    mp_tx_head++;                            // publish after copy

    if (!mp_tx_scheduled) {
        mp_tx_scheduled = true;
        mp_tx_cb.callback = &mp_tx_pump;
        mp_tx_cb.context  = NULL;
        btstack_run_loop_execute_on_main_thread(&mp_tx_cb);
    }
    return true;
}

// Forget the connected MouthPad's bond (the utility's clear_bonds relay command).
// forget_device touches gap_disconnect + le_device_db, so it must run on the
// BTstack thread — marshal it like the NUS write. Returns true if a connected
// MouthPad was captured for removal (the response success).
static btstack_context_callback_registration_t mp_clearbond_cb;
static bd_addr_t mp_clearbond_addr;
static volatile bool mp_clearbond_pending;

static void mp_clearbond_run(void* ctx)
{
    (void)ctx;
    mp_clearbond_pending = false;
    btstack_host_forget_device(mp_clearbond_addr);   // now on the BTstack thread
}

bool btstack_host_mouthpad_clear_bond(void)
{
    if (mp_nus.handle == HCI_CON_HANDLE_INVALID) return false;
    ble_connection_t* c = find_connection_by_handle(mp_nus.handle);
    if (!c) return false;
    memcpy(mp_clearbond_addr, c->addr, 6);
    if (!mp_clearbond_pending) {
        mp_clearbond_pending = true;
        mp_clearbond_cb.callback = &mp_clearbond_run;
        mp_clearbond_cb.context  = NULL;
        btstack_run_loop_execute_on_main_thread(&mp_clearbond_cb);
    }
    return true;
}

// ============================================================================
// VALVE STEAM CONTROLLER 2 ("Triton") CUSTOM GATT CLIENT
// ============================================================================
// The SC2 in Bluetooth mode does NOT use HID-over-GATT. It exposes Valve's
// proprietary GATT service (same 100F6C32-… service as the 2015 controller)
// with different characteristics:
//   input char 100F6C7A → report id 0x45   (TritonMTUNoQuat)
//   input char 100F6C7C → report id 0x47   (TritonMTUNoQuat32TS / "Ibex")
//   report/feature char 100F6C34           (feature-report writes)
// Only ONE of the two input characteristics is present on a given unit; we
// subscribe to whichever we find. Notifications carry the report body WITHOUT
// the report-id byte (it is implied by the characteristic), so we prepend it
// and hand a normal 46-byte HID report to the bthid layer via bt_on_hid_report.
//
// This is a discovery-by-UUID128 client (like the MouthPad NUS client) that
// routes into a bthid_driver_t (like the Switch 2 fast path). All GATT ops run
// on the BTstack thread. Ref: SDL src/joystick/hidapi/SDL_hidapi_steam_triton.c
// and android HIDDeviceBLESteamController.java.

// 128-bit UUIDs in textual/big-endian order (as BTstack uuid128 expects).
static const uint8_t valve_service_uuid128[16] = {
    0x10,0x0F,0x6C,0x32,0x17,0x35,0x43,0x13,0xB4,0x02,0x38,0x56,0x71,0x31,0xE5,0xF3};
static const uint8_t valve_input_45_uuid128[16] = {  // notify, report id 0x45
    0x10,0x0F,0x6C,0x7A,0x17,0x35,0x43,0x13,0xB4,0x02,0x38,0x56,0x71,0x31,0xE5,0xF3};
static const uint8_t valve_input_47_uuid128[16] = {  // notify, report id 0x47
    0x10,0x0F,0x6C,0x7C,0x17,0x35,0x43,0x13,0xB4,0x02,0x38,0x56,0x71,0x31,0xE5,0xF3};
static const uint8_t valve_input_33_uuid128[16] = {  // notify, original Steam Controller (SC1) BLE input
    0x10,0x0F,0x6C,0x33,0x17,0x35,0x43,0x13,0xB4,0x02,0x38,0x56,0x71,0x31,0xE5,0xF3};
static const uint8_t valve_report_uuid128[16] = {    // write (feature reports)
    0x10,0x0F,0x6C,0x34,0x17,0x35,0x43,0x13,0xB4,0x02,0x38,0x56,0x71,0x31,0xE5,0xF3};
static const uint8_t valve_haptic_rumble_uuid128[16] = {  // WWR, output report 0x80 (0x80+0x35=0xB5)
    0x10,0x0F,0x6C,0xB5,0x17,0x35,0x43,0x13,0xB4,0x02,0x38,0x56,0x71,0x31,0xE5,0xF3};
static const uint8_t valve_battery_uuid128[16] = {  // notify, battery report 0x43 (0x43+0x35=0x78)
    0x10,0x0F,0x6C,0x78,0x17,0x35,0x43,0x13,0xB4,0x02,0x38,0x56,0x71,0x31,0xE5,0xF3};

// Feature-report payloads (report-id byte already stripped, as sent on air).
// {0x87 = ID_SET_SETTINGS_VALUES, 0x03 = sizeof(ControllerSetting), setting, u16 value}
// Non-const: gatt_client_write_value_of_characteristic() takes a non-const ptr.
static uint8_t valve_setting_lizard_off[5] = { 0x87, 0x03, 0x09, 0x00, 0x00 };
// IMU on: SEND_RAW_ACCEL(0x08) | SEND_RAW_GYRO(0x10) = 0x18
static uint8_t valve_setting_imu_on[5]     = { 0x87, 0x03, 0x30, 0x18, 0x00 };

#define VALVE_KEEPALIVE_MS 3000   // controller re-enables lizard mode on a watchdog

typedef enum {
    VALVE_IDLE = 0,
    VALVE_DISC_SERVICE,
    VALVE_DISC_CHARS,
    VALVE_ENABLE_CCC,
    VALVE_ENABLE_BATT_CCC,
    VALVE_DISC_HID_SVC,     // discover the standard HID service (0x1812)
    VALVE_DISC_HID_CHARS,   // discover its HID Report characteristics (0x2A4D)
    VALVE_ENABLE_HID_CCC,   // subscribe each input-report char (powers on the pads)
    VALVE_READY,
} valve_state_t;

static struct {
    valve_state_t state;
    hci_con_handle_t handle;
    uint8_t conn_index;
    gatt_client_service_t service;
    gatt_client_characteristic_t input_char;   // subscribed notify characteristic
    gatt_client_characteristic_t battery_char; // battery notify characteristic (100F6C78)
    uint8_t report_id;                          // 0x45 or 0x47 (implied by input_char)
    bool has_battery_char;                      // battery characteristic discovered
    uint16_t report_value_handle;               // feature-report write target (100F6C34)
    uint16_t rumble_value_handle;               // WWR output report 0x80 target (100F6CB5)
    gatt_client_notification_t notify;
    gatt_client_notification_t battery_notify;
    // Standard HID service (0x1812): subscribing to its input reports powers on the
    // trackpads (dead on the Valve vendor stream) and carries their position. Runs
    // alongside the Valve client (which owns connection/lizard-off/rumble/battery).
    gatt_client_service_t hid_service;
    bool has_hid_service;
    gatt_client_characteristic_t hid_reports[8];
    gatt_client_notification_t hid_notify[8];
    uint8_t hid_report_count;
    uint8_t hid_ccc_index;
    uint16_t hid_protocol_handle;   // HID Protocol Mode char (0x2A4E) — set REPORT
    uint32_t hid_disc_start_ms;     // when HID-service bring-up began (timeout guard)
    uint32_t start_ms;              // when the whole Valve bring-up began (global timeout guard)
    uint8_t stall_retries;         // consecutive bring-up stalls (escalate after a few)
    bool need_start_disc;          // initial service discovery pending (defer until gatt_client ready)
    uint32_t last_keepalive_ms;
    bool imu_enabled;
    uint8_t last_rumble;         // last intensity sent (0 = idle)
    uint32_t last_rumble_ms;     // last rumble write time
} valve = { .state = VALVE_IDLE, .handle = HCI_CON_HANDLE_INVALID };

static void valve_reset(void)
{
    if (valve.state == VALVE_READY) {
        gatt_client_stop_listening_for_characteristic_value_updates(&valve.notify);
        if (valve.has_battery_char) {
            gatt_client_stop_listening_for_characteristic_value_updates(&valve.battery_notify);
        }
        for (int i = 0; i < valve.hid_report_count; i++) {
            gatt_client_stop_listening_for_characteristic_value_updates(&valve.hid_notify[i]);
        }
    }
    valve.state = VALVE_IDLE;
    valve.handle = HCI_CON_HANDLE_INVALID;
    valve.report_id = 0;
    valve.has_battery_char = false;
    valve.has_hid_service = false;
    valve.hid_report_count = 0;
    valve.hid_ccc_index = 0;
    valve.hid_protocol_handle = 0;
    valve.report_value_handle = 0;
    valve.rumble_value_handle = 0;
    valve.imu_enabled = false;
    valve.need_start_disc = false;
    valve.last_rumble = 0;
    valve.last_rumble_ms = 0;
}

static void valve_disconnected(hci_con_handle_t handle)
{
    if (valve.handle == handle) {
        printf("[SC2_BLE] disconnected — Valve GATT client reset\n");
        valve_reset();
    }
}

// Device -> host input notifications on the Valve input characteristic.
static void valve_notify_handler(uint8_t packet_type, uint16_t channel, uint8_t* packet, uint16_t size)
{
    UNUSED(channel); UNUSED(size);
    if (packet_type != HCI_EVENT_PACKET) return;
    if (hci_event_packet_get_type(packet) != GATT_EVENT_NOTIFICATION) return;
    uint16_t vh = gatt_event_notification_get_value_handle(packet);
    if (vh != valve.input_char.value_handle) return;

    uint16_t len = gatt_event_notification_get_value_length(packet);
    const uint8_t* val = gatt_event_notification_get_value(packet);

    // Prepend the implied report id, then defer to the main loop (route via
    // bt_on_hid_report) to keep the BTstack callback stack shallow. The SC1 (0xC1)
    // sends short delta-compressed reports, so it uses a smaller minimum length; the
    // SC2 requires its full state report.
    uint16_t min_len = (valve.report_id == 0xC1) ? 4 : 18;
    if (len < min_len || len + 1 > (uint16_t)sizeof(pending_ble_report)) return;
    pending_ble_report[0] = valve.report_id;
    memcpy(pending_ble_report + 1, val, len);
    pending_ble_report_len = len + 1;
    pending_ble_conn_index = valve.conn_index;
    ble_report_pending = true;
}

// Battery notifications on the Valve battery characteristic (100F6C78). Over BLE
// the report-id byte is stripped, so the value is the TritonBatteryStatus payload:
// byte 0 = charge state (2=charging, 4=charge-done), byte 1 = level 0-100.
static void valve_battery_notify_handler(uint8_t packet_type, uint16_t channel, uint8_t* packet, uint16_t size)
{
    UNUSED(channel); UNUSED(size);
    if (packet_type != HCI_EVENT_PACKET) return;
    if (hci_event_packet_get_type(packet) != GATT_EVENT_NOTIFICATION) return;
    uint16_t vh = gatt_event_notification_get_value_handle(packet);
    if (vh != valve.battery_char.value_handle) return;

    uint16_t len = gatt_event_notification_get_value_length(packet);
    const uint8_t* val = gatt_event_notification_get_value(packet);
    if (len < 2) return;
    uint8_t state = val[0];
    uint8_t level = val[1];
    bool charging = (state == 2 || state == 4);
    bthid_set_battery(valve.conn_index, level, charging);
}

// Feature-report write completion (with-response). Log only.
static void valve_feature_write_cb(uint8_t packet_type, uint16_t channel, uint8_t* packet, uint16_t size)
{
    UNUSED(channel); UNUSED(size);
    if (packet_type != HCI_EVENT_PACKET) return;
    if (hci_event_packet_get_type(packet) != GATT_EVENT_QUERY_COMPLETE) return;
    uint8_t status = gatt_event_query_complete_get_att_status(packet);
    if (status != ATT_ERROR_SUCCESS) {
        printf("[SC2_BLE] feature write failed status=0x%02X\n", status);
    }
}

// Standard-HID-service (0x1812) input reports. We subscribe to these purely to
// power on the trackpads — the controller only enables the pads (and streams their
// position on the Valve 0x45 report) once a host consumes this HID service. The
// HID reports themselves carry nothing we need, so this handler just drops them.
static void valve_hid_notify_handler(uint8_t packet_type, uint16_t channel, uint8_t* packet, uint16_t size)
{
    UNUSED(packet_type); UNUSED(channel); UNUSED(packet); UNUSED(size);
}

// Finish Valve bring-up: listen on the input characteristic, register with the
// bthid layer so the SC2 driver is selected, and mark READY.
static void valve_become_ready(void)
{
    gatt_client_listen_for_characteristic_value_updates(
        &valve.notify, valve_notify_handler, valve.handle, &valve.input_char);
    valve.state = VALVE_READY;
    valve.stall_retries = 0;
    valve.last_keepalive_ms = btstack_run_loop_get_time_ms();
    printf("[SC2_BLE] ready (report id 0x%02X, battery %s)\n",
           valve.report_id, valve.has_battery_char ? "yes" : "no");
    if (valve.report_id == 0xC1) {
        // Original Steam Controller BLE firmware requests a slow (~1s) connection
        // interval to save power, which throttles input to ~1 packet/sec and stalls
        // on a burst of state (a button press). As the central, force a fast gamepad
        // interval (7.5-15ms). Re-asserted on a timer in valve_periodic because the
        // controller re-requests the slow interval after connecting.
        gap_update_connection_parameters(valve.handle, 6, 12, 0, 400);
    }

    // Register the device with the bthid layer so the SC2 driver is selected and
    // reports route to it. VID/PID are synthetic (not advertised over the air) so
    // driver match() has something stable.
    ble_connection_t* conn = find_connection_by_handle(valve.handle);
    if (conn) {
        conn->vid = 0x28DE;
        // Distinct synthetic PID for the original Steam Controller (report id 0xC1) so
        // its dedicated BLE driver matches instead of the SC2's.
        conn->pid = (valve.report_id == 0xC1) ? 0x1101 : 0x1303;
        bthid_update_device_info(valve.conn_index, conn->name, conn->vid, conn->pid);
        btstack_host_stop_scan();
        scan_timeout_end = 0;
        bt_on_hid_ready(valve.conn_index);
    }
}

// GATT discovery state machine for the Valve service.
static void valve_gatt_handler(uint8_t packet_type, uint16_t channel, uint8_t* packet, uint16_t size)
{
    UNUSED(channel); UNUSED(size);
    if (packet_type != HCI_EVENT_PACKET) return;
    uint8_t event = hci_event_packet_get_type(packet);

    switch (event) {
        case GATT_EVENT_SERVICE_QUERY_RESULT:
            if (valve.state == VALVE_DISC_HID_SVC) {
                gatt_event_service_query_result_get_service(packet, &valve.hid_service);
                valve.has_hid_service = true;
            } else {
                gatt_event_service_query_result_get_service(packet, &valve.service);
            }
            break;

        case GATT_EVENT_CHARACTERISTIC_QUERY_RESULT: {
            gatt_client_characteristic_t ch;
            gatt_event_characteristic_query_result_get_characteristic(packet, &ch);
            if (valve.state == VALVE_DISC_HID_CHARS) {
                // Collect HID Report (0x2A4D) chars with NOTIFY (input reports) —
                // subscribing to these wakes the pads. Also grab the Protocol Mode
                // char (0x2A4E) so we can switch the device to REPORT mode (else it
                // stays in Boot mode and only sends keyboard/mouse, not the full
                // gamepad+trackpad reports).
                if (ch.uuid16 == 0x2A4D && (ch.properties & 0x10) &&
                    valve.hid_report_count < 8) {
                    valve.hid_reports[valve.hid_report_count++] = ch;
                } else if (ch.uuid16 == 0x2A4E) {
                    valve.hid_protocol_handle = ch.value_handle;
                }
                break;
            }
            if (memcmp(ch.uuid128, valve_input_45_uuid128, 16) == 0) {
                valve.input_char = ch;
                valve.report_id = 0x45;
            } else if (memcmp(ch.uuid128, valve_input_47_uuid128, 16) == 0) {
                valve.input_char = ch;
                valve.report_id = 0x47;
            } else if (memcmp(ch.uuid128, valve_input_33_uuid128, 16) == 0) {
                valve.input_char = ch;
                valve.report_id = 0xC1;   // marker: original Steam Controller (SC1) BLE
            } else if (memcmp(ch.uuid128, valve_report_uuid128, 16) == 0) {
                valve.report_value_handle = ch.value_handle;
            } else if (memcmp(ch.uuid128, valve_haptic_rumble_uuid128, 16) == 0) {
                valve.rumble_value_handle = ch.value_handle;
            } else if (memcmp(ch.uuid128, valve_battery_uuid128, 16) == 0) {
                valve.battery_char = ch;
                valve.has_battery_char = true;
            }
            break;
        }

        case GATT_EVENT_QUERY_COMPLETE: {
            uint8_t status = gatt_event_query_complete_get_att_status(packet);
            if (status != ATT_ERROR_SUCCESS) {
                // A failed battery CCC is non-fatal — battery is optional; keep the
                // controller and go ready without battery notifications.
                if (valve.state == VALVE_ENABLE_BATT_CCC) {
                    printf("[SC2_BLE] battery CCC failed (0x%02X), continuing without battery\n", status);
                    valve.has_battery_char = false;
                    valve.state = VALVE_DISC_HID_SVC;
                    valve.hid_disc_start_ms = btstack_run_loop_get_time_ms();
                    valve.has_hid_service = false;
                    gatt_client_discover_primary_services_by_uuid16(
                        valve_gatt_handler, valve.handle, 0x1812);
                    break;
                }
                // HID-service steps are optional (trackpads only) — never drop the
                // working controller over them.
                if (valve.state == VALVE_DISC_HID_SVC || valve.state == VALVE_DISC_HID_CHARS ||
                    valve.state == VALVE_ENABLE_HID_CCC) {
                    printf("[SC2_BLE] HID service step failed (state=%d 0x%02X), no trackpads\n",
                           valve.state, status);
                    valve_become_ready();
                    break;
                }
                printf("[SC2_BLE] GATT query failed (state=%d status=0x%02X)\n", valve.state, status);
                valve_reset();
                break;
            }
            if (valve.state == VALVE_DISC_SERVICE) {
                if (valve.service.start_group_handle == 0) {
                    printf("[SC2_BLE] No Valve service on device\n");
                    valve_reset();
                    break;
                }
                valve.state = VALVE_DISC_CHARS;
                gatt_client_discover_characteristics_for_service(
                    valve_gatt_handler, valve.handle, &valve.service);
            } else if (valve.state == VALVE_DISC_CHARS) {
                if (valve.report_id == 0 || valve.input_char.value_handle == 0) {
                    printf("[SC2_BLE] No Valve input characteristic found\n");
                    valve_reset();
                    break;
                }
                printf("[SC2_BLE] input char 0x%02X (handle 0x%04X), report char handle 0x%04X\n",
                       valve.report_id, valve.input_char.value_handle, valve.report_value_handle);
                valve.state = VALVE_ENABLE_CCC;
                gatt_client_write_client_characteristic_configuration(
                    valve_gatt_handler, valve.handle, &valve.input_char,
                    GATT_CLIENT_CHARACTERISTICS_CONFIGURATION_NOTIFICATION);
            } else if (valve.state == VALVE_ENABLE_CCC) {
                // Input CCC done. Subscribe the battery characteristic too (if the
                // unit exposes one) before going ready; otherwise ready now.
                if (valve.has_battery_char) {
                    valve.state = VALVE_ENABLE_BATT_CCC;
                    gatt_client_write_client_characteristic_configuration(
                        valve_gatt_handler, valve.handle, &valve.battery_char,
                        GATT_CLIENT_CHARACTERISTICS_CONFIGURATION_NOTIFICATION);
                } else {
                    valve_become_ready();
                }
            } else if (valve.state == VALVE_ENABLE_BATT_CCC) {
                gatt_client_listen_for_characteristic_value_updates(
                    &valve.battery_notify, valve_battery_notify_handler,
                    valve.handle, &valve.battery_char);
                // Discover the standard HID service (0x1812) and subscribe its input
                // reports — this powers on the trackpads and carries them.
                valve.state = VALVE_DISC_HID_SVC;
                valve.hid_disc_start_ms = btstack_run_loop_get_time_ms();
                valve.has_hid_service = false;
                gatt_client_discover_primary_services_by_uuid16(
                    valve_gatt_handler, valve.handle, 0x1812);
            } else if (valve.state == VALVE_DISC_HID_SVC) {
                if (!valve.has_hid_service) {
                    printf("[SC2_BLE] no standard HID service — no trackpads\n");
                    valve_become_ready();
                    break;
                }
                valve.state = VALVE_DISC_HID_CHARS;
                valve.hid_report_count = 0;
                gatt_client_discover_characteristics_for_service(
                    valve_gatt_handler, valve.handle, &valve.hid_service);
            } else if (valve.state == VALVE_DISC_HID_CHARS) {
                printf("[SC2_BLE] HID service: %d input-report chars\n", valve.hid_report_count);
                // NOTE: do NOT write Protocol Mode here — a write-without-response
                // leaves the gatt_client busy and the CCC write below silently fails,
                // hanging bring-up before READY. It isn't needed anyway: subscribing
                // the input reports (CCC) is what wakes the pads; their position rides
                // the Valve 0x45 stream, not these HID reports.
                if (valve.hid_report_count == 0) { valve_become_ready(); break; }
                valve.hid_ccc_index = 0;
                valve.state = VALVE_ENABLE_HID_CCC;
                // If the write errors SYNCHRONOUSLY (busy/wrong-state) no QUERY_COMPLETE
                // ever fires — that would hang bring-up. Fall through to READY instead.
                if (gatt_client_write_client_characteristic_configuration(
                        valve_gatt_handler, valve.handle, &valve.hid_reports[0],
                        GATT_CLIENT_CHARACTERISTICS_CONFIGURATION_NOTIFICATION) != ERROR_CODE_SUCCESS) {
                    printf("[SC2_BLE] HID CCC write rejected — no trackpads\n");
                    valve_become_ready();
                }
            } else if (valve.state == VALVE_ENABLE_HID_CCC) {
                gatt_client_listen_for_characteristic_value_updates(
                    &valve.hid_notify[valve.hid_ccc_index], valve_hid_notify_handler,
                    valve.handle, &valve.hid_reports[valve.hid_ccc_index]);
                valve.hid_ccc_index++;
                if (valve.hid_ccc_index < valve.hid_report_count) {
                    if (gatt_client_write_client_characteristic_configuration(
                            valve_gatt_handler, valve.handle,
                            &valve.hid_reports[valve.hid_ccc_index],
                            GATT_CLIENT_CHARACTERISTICS_CONFIGURATION_NOTIFICATION) != ERROR_CODE_SUCCESS) {
                        printf("[SC2_BLE] HID CCC write rejected mid-chain — pads best-effort\n");
                        valve_become_ready();
                    }
                } else {
                    printf("[SC2_BLE] HID input reports subscribed (%d) — pads should wake\n",
                           valve.hid_report_count);
                    valve_become_ready();
                }
            }
            break;
        }
    }
}

static void register_valve_hid_listener(hci_con_handle_t con_handle)
{
    printf("[SC2_BLE] Registering Valve GATT client for handle 0x%04X\n", con_handle);

    ble_connection_t* conn = find_connection_by_handle(con_handle);
    if (!conn) {
        printf("[SC2_BLE] ERROR: No connection for handle 0x%04X\n", con_handle);
        return;
    }

    int ble_index = -1;
    for (int i = 0; i < MAX_BLE_CONNECTIONS; i++) {
        if (&hid_state.connections[i] == conn) { ble_index = i; break; }
    }
    if (ble_index < 0) return;

    conn->conn_index = BLE_CONN_INDEX_OFFSET + ble_index;
    conn->hid_ready = true;

    valve_reset();
    valve.handle = con_handle;
    valve.conn_index = conn->conn_index;
    valve.state = VALVE_DISC_SERVICE;
    valve.start_ms = btstack_run_loop_get_time_ms();
    valve.service.start_group_handle = 0;

    // Start service discovery — but NOT blindly. On a bonded reconnect this fires
    // straight off SM re-encryption, when the gatt_client for this handle can still
    // be busy/settling; the discover call is then rejected (GATT_CLIENT_IN_WRONG_STATE)
    // and — because the return was historically ignored — no QUERY_COMPLETE ever came,
    // hanging bring-up at DISC_SERVICE (VID stays 0000, no input, controller powers off).
    // That's the fresh-pair-works / cold-reconnect-dies bug. Defer to valve_periodic,
    // which retries once gatt_client_is_ready(). (Fresh pair is ready by now, so it
    // starts on this very call.)
    uint8_t rc = gatt_client_discover_primary_services_by_uuid128(
        valve_gatt_handler, con_handle, valve_service_uuid128);
    if (rc != ERROR_CODE_SUCCESS) {
        printf("[SC2_BLE] initial discovery deferred (rc=0x%02X, gatt_client not ready)\n", rc);
        valve.need_start_disc = true;
    }
}

// SC2 rumble over BLE — output report 0x80 (ID_OUT_REPORT_HAPTIC_RUMBLE), sent as
// an ATT Write Without Response (HID output-report semantics; a Write Request draws
// ATT_ERROR_INVALID_PDU from the SC2). left/right speed carry the magnitude. An
// active rumble is re-sent every ~40ms to ride the controller's safety timeout.
// Returns true if it issued a GATT write this tick (so the caller yields the slot).
#define VALVE_RUMBLE_RESEND_MS 40
static bool valve_handle_rumble(void)
{
    int player_idx = find_player_index(valve.conn_index, 0);
    feedback_state_t* fb = (player_idx >= 0) ? feedback_get_state(player_idx) : NULL;
    uint8_t intensity = fb ? (fb->rumble.left > fb->rumble.right ? fb->rumble.left : fb->rumble.right) : 0;
    uint32_t now = btstack_run_loop_get_time_ms();

    if (!fb) return false;
    if (valve.rumble_value_handle == 0) return false;   // no haptic characteristic discovered
    bool changed = (intensity != valve.last_rumble);
    bool refresh = (intensity > 0 && (now - valve.last_rumble_ms) >= VALVE_RUMBLE_RESEND_MS);
    if (!changed && !refresh) return false;

    valve.last_rumble = intensity;
    valve.last_rumble_ms = now;

    // SDL SC2/triton rumble = output report 0x80 (ID_OUT_REPORT_HAPTIC_RUMBLE). Over
    // the Valve BLE service each report has its own characteristic (0x80 -> 100F6CB5)
    // and the leading report-id byte is stripped (as with the input chars), so the
    // written value is just the payload:
    //   [type=0, intensity_u16=0, left.speed_u16, left.gain=0, right.speed_u16,
    //    right.gain=0]  (all LE). Magnitude rides entirely on left/right.speed (SDL
    //   passes the 0..65535 rumble value straight through); gain stays 0 dB; speed 0
    //   on both = stop. It is an output report -> ATT Write Without Response (the
    //   haptic char is WWR-capable; feature writes to 0x0017 use Write Request).
    uint16_t left_speed  = (uint16_t)(fb->rumble.left  * 257);  // 0..255 -> 0..65535
    uint16_t right_speed = (uint16_t)(fb->rumble.right * 257);
    static uint8_t rmb[9];
    rmb[0] = 0x00;                                              // type
    rmb[1] = 0x00; rmb[2] = 0x00;                              // intensity (unused)
    rmb[3] = (uint8_t)(left_speed & 0xFF);  rmb[4] = (uint8_t)(left_speed >> 8);
    rmb[5] = 0x00;                                              // left gain (0 dB)
    rmb[6] = (uint8_t)(right_speed & 0xFF); rmb[7] = (uint8_t)(right_speed >> 8);
    rmb[8] = 0x00;                                              // right gain (0 dB)
    gatt_client_write_value_of_characteristic_without_response(
        valve.handle, valve.rumble_value_handle, sizeof(rmb), rmb);
    return true;
}

// Periodic: keep gamepad mode alive. The controller re-enables "lizard mode"
// (keyboard/mouse emulation) on a ~3 s watchdog, so we resend the lizard-off
// feature report at that cadence. IMU is enabled once, rumble is driven from the
// feedback system. All on the BTstack thread, one GATT op at a time.
// Identify-stall watchdog. A BLE link that comes up unidentified (no name match →
// generic HOGP path) only has its DIS (VID/PID) read once the generic HIDS client
// reaches REPORTS_NOTIFICATION (0x1C). A Steam Controller 2 in lizard mode never
// reaches that on the generic path — its adv name is inconsistent, so it sometimes
// misses the "Steam" gate, lands on generic HOGP, streams no reports, VID stays 0,
// and the DIS-reactive Valve switch (which needs the VID) never fires. Result: stuck
// "Generic BLE Gamepad", no buttons, permanently. After a grace window, force a
// one-shot DIS read on any still-unidentified link so that switch can identify the
// SC2 (VID 0x28DE) and hand it to the Valve client. Harmless for other devices:
// their VID just gets populated and they stay on the generic path.
#define BLE_IDENTIFY_STALL_MS 3500
static void ble_identify_stall_task(void)
{
    uint32_t now = btstack_run_loop_get_time_ms();
    for (int i = 0; i < MAX_BLE_CONNECTIONS; i++) {
        ble_connection_t* c = &hid_state.connections[i];
        if (c->handle == HCI_CON_HANDLE_INVALID) continue;   // slot unused
        if (c->connect_ms == 0) continue;
        if (c->dis_probe_started) continue;
        if (c->vid != 0) continue;                           // already identified
        if (c->profile && c->profile->ble == BT_BLE_VALVE) continue; // valve owns it
        if ((now - c->connect_ms) < BLE_IDENTIFY_STALL_MS) continue;
        if (!gatt_client_is_ready(c->handle)) continue;      // don't collide with an op in flight
        printf("[BTSTACK_HOST] link 0x%04X unidentified after %ums — forcing DIS read\n",
               c->handle, (unsigned)(now - c->connect_ms));
        uint8_t st = device_information_service_client_query(c->handle, dis_client_handler);
        if (st == ERROR_CODE_SUCCESS) {
            c->dis_probe_started = true;  // one-shot; DIS handler drives the rest
        } else {
            printf("[BTSTACK_HOST] forced DIS query rejected (0x%02X) — will retry\n", st);
        }
    }
}

static void valve_periodic(void)
{
    // SC1 BLE: re-assert the fast connection interval. Its firmware re-requests a slow
    // ~1s interval after connecting (throttling input to a "1s drip" and stalling on a
    // button burst); as the central we push it back to a gamepad interval on a timer.
    if (valve.report_id == 0xC1 && valve.state == VALVE_READY &&
        valve.handle != HCI_CON_HANDLE_INVALID) {
        static uint32_t last_cparam_ms = 0;
        uint32_t nowc = btstack_run_loop_get_time_ms();
        if (last_cparam_ms == 0 || (nowc - last_cparam_ms) >= 3000) {
            last_cparam_ms = nowc;
            gap_update_connection_parameters(valve.handle, 6, 12, 0, 400);
        }
    }

    // Deferred initial discovery: on a bonded reconnect the discover call in
    // register_valve_hid_listener may be rejected because the gatt_client is still
    // settling right after re-encryption. Retry here once it's ready so bring-up
    // actually starts instead of hanging silently at DISC_SERVICE.
    if (valve.need_start_disc && valve.handle != HCI_CON_HANDLE_INVALID) {
        if (gatt_client_is_ready(valve.handle)) {
            uint8_t rc = gatt_client_discover_primary_services_by_uuid128(
                valve_gatt_handler, valve.handle, valve_service_uuid128);
            if (rc == ERROR_CODE_SUCCESS) {
                printf("[SC2_BLE] deferred discovery started\n");
                valve.need_start_disc = false;
            }
        }
        return;  // nothing else to do until discovery is under way
    }

    // Robustness: the optional HID-service bring-up (trackpad wake) adds several
    // GATT ops; if any stalls without an error event, the state machine would hang
    // before READY and the controller would never register. Guard it — after ~4s
    // stuck in a HID bring-up state, give up on trackpads and go READY so buttons/
    // rumble/battery always come up.
    if (valve.handle != HCI_CON_HANDLE_INVALID &&
        (valve.state == VALVE_DISC_HID_SVC || valve.state == VALVE_DISC_HID_CHARS ||
         valve.state == VALVE_ENABLE_HID_CCC) &&
        (btstack_run_loop_get_time_ms() - valve.hid_disc_start_ms) > 4000) {
        printf("[SC2_BLE] HID bring-up timed out — going ready without trackpads\n");
        valve.hid_report_count = 0;
        valve_become_ready();
    }

    // Global bring-up guard: if any EARLY Valve step (service/char discovery, input or
    // battery CCC) stalls with no GATT event — nothing above rescues it, so the SC2
    // hangs at VID:0000, passes no input, and eventually powers off. After ~9s stuck
    // in any pre-READY state, log exactly where it hung (for CDC diagnosis) and drop
    // the link so it re-advertises and we retry from a clean GATT state.
    if (valve.handle != HCI_CON_HANDLE_INVALID &&
        valve.state != VALVE_READY && valve.state != VALVE_IDLE &&
        (btstack_run_loop_get_time_ms() - valve.start_ms) > 9000) {
        hci_con_handle_t stuck = valve.handle;
        valve.stall_retries++;
        printf("[SC2_BLE] bring-up STUCK at state %d after 9s (retry #%u) — disconnecting to retry\n",
               valve.state, valve.stall_retries);
        valve_reset();               // tear down the half-built Valve client
        gap_disconnect(stuck);       // force re-advertise → fresh reconnect + discovery
        return;
    }

    if (valve.state != VALVE_READY) return;
    if (valve.report_value_handle == 0) return;          // no feature char — input still works
    if (gatt_client_is_ready(valve.handle) == 0) return; // another GATT op in flight

    // Original Steam Controller (SC1) BLE lizard-off differs from the SC2: it needs
    // clear-digital-mappings (0x81) + trackpad-mode settings (0x87 reg 7/8), NOT the
    // SC2's SETTING_LIZARD_MODE (reg 9) which it ignores (leaving it in lizard/mouse
    // mode — the all-zero 0xC0 reports). Clear mappings once, re-send settings on the
    // keepalive.
    if (valve.report_id == 0xC1) {
        // SC1 BLE writes are one 19-byte segment: [0xC0 = seg0|last][18-byte payload].
        // (The 0x03 HID report number is implicit over GATT — the notifications we get
        // back are 19 bytes starting 0xC0, no 0x03, so the write matches that.) The
        // payload must be padded to the full segment; a short write draws ATT 0x1F.
        //   Clear digital mappings (0x81) — stop keyboard emulation.
        static uint8_t sc1_clear[19] = { 0xC0, 0x81 };
        //   ID_SET_SETTINGS_VALUES (0x87), 3 settings × 3 bytes (len 0x09):
        //     SETTING_WIRELESS_PACKET_VERSION (49) = 2  -> ENABLES full STATE reports
        //     SETTING_LEFT_TRACKPAD_MODE  (7) = TRACKPAD_NONE (7)  -> no mouse/lizard
        //     SETTING_RIGHT_TRACKPAD_MODE (8) = TRACKPAD_NONE (7)
        static uint8_t sc1_settings[19] = {
            0xC0, 0x87, 0x09,
            49, 0x02, 0x00,
            7,  0x07, 0x00,
            8,  0x07, 0x00 };
        if (!valve.imu_enabled) {
            valve.imu_enabled = true;   // reuse as "settings sent"
            // Enable STATE reports once (persists). Also clears trackpad lizard modes.
            gatt_client_write_value_of_characteristic(
                valve_feature_write_cb, valve.handle, valve.report_value_handle,
                sizeof(sc1_settings), sc1_settings);
            return;
        }
        uint32_t now1 = btstack_run_loop_get_time_ms();
        if ((now1 - valve.last_keepalive_ms) >= VALVE_KEEPALIVE_MS) {
            valve.last_keepalive_ms = now1;
            // Re-send clear-mappings on the keepalive: the controller's watchdog
            // re-enables the keyboard button mapping, which steals the buttons from
            // the gamepad STATE report (empty C0 04). Keep them cleared.
            gatt_client_write_value_of_characteristic(
                valve_feature_write_cb, valve.handle, valve.report_value_handle,
                sizeof(sc1_clear), sc1_clear);
        }
        return;
    }

    if (!valve.imu_enabled) {
        valve.imu_enabled = true;
        gatt_client_write_value_of_characteristic(
            valve_feature_write_cb, valve.handle, valve.report_value_handle,
            sizeof(valve_setting_imu_on), valve_setting_imu_on);
        return;
    }

    // Rumble takes priority (40ms cadence); yields the GATT slot if it wrote.
    if (valve_handle_rumble()) return;

    uint32_t now = btstack_run_loop_get_time_ms();
    if ((now - valve.last_keepalive_ms) >= VALVE_KEEPALIVE_MS) {
        valve.last_keepalive_ms = now;
        gatt_client_write_value_of_characteristic(
            valve_feature_write_cb, valve.handle, valve.report_value_handle,
            sizeof(valve_setting_lizard_off), valve_setting_lizard_off);
    }
}

// ============================================================================
// GATT CLIENT
// ============================================================================

static void gatt_client_callback(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size)
{
    UNUSED(channel);
    UNUSED(size);

    if (packet_type != HCI_EVENT_PACKET) return;

    uint8_t event_type = hci_event_packet_get_type(packet);

    switch (event_type) {
        case GATT_EVENT_SERVICE_QUERY_RESULT: {
            gatt_client_service_t service;
            gatt_event_service_query_result_get_service(packet, &service);
            printf("[BTSTACK_HOST] GATT: Service 0x%04X-0x%04X UUID=0x%04X\n",
                   service.start_group_handle, service.end_group_handle,
                   service.uuid16);
            // Save HID service handles (UUID 0x1812)
            if (service.uuid16 == 0x1812) {
                hid_state.hid_service_start = service.start_group_handle;
                hid_state.hid_service_end = service.end_group_handle;
                printf("[BTSTACK_HOST] Found HID Service!\n");
            }
            break;
        }

        case GATT_EVENT_CHARACTERISTIC_QUERY_RESULT: {
            gatt_client_characteristic_t characteristic;
            gatt_event_characteristic_query_result_get_characteristic(packet, &characteristic);
            printf("[BTSTACK_HOST] GATT: Char handle=0x%04X value=0x%04X end=0x%04X props=0x%02X UUID=0x%04X\n",
                   characteristic.start_handle, characteristic.value_handle,
                   characteristic.end_handle, characteristic.properties, characteristic.uuid16);
            // Save first Report characteristic (UUID 0x2A4D) with Notify property
            if (characteristic.uuid16 == 0x2A4D && (characteristic.properties & 0x10) &&
                hid_state.report_characteristic.value_handle == 0) {
                hid_state.report_characteristic = characteristic;
                printf("[BTSTACK_HOST] Found HID Report characteristic!\n");
            }
            break;
        }

        case GATT_EVENT_QUERY_COMPLETE: {
            uint8_t status = gatt_event_query_complete_get_att_status(packet);
            printf("[BTSTACK_HOST] GATT: Query complete, status=0x%02X, gatt_state=%d\n",
                   status, hid_state.gatt_state);

            if (status != 0) break;

            // State machine for GATT discovery
            if (hid_state.gatt_state == GATT_DISCOVERING_SERVICES) {
                if (hid_state.hid_service_start != 0) {
                    // Found HID, now discover its characteristics
                    printf("[BTSTACK_HOST] Discovering HID characteristics...\n");
                    hid_state.gatt_state = GATT_DISCOVERING_HID_CHARACTERISTICS;
                    gatt_client_discover_characteristics_for_handle_range_by_uuid16(
                        gatt_client_callback, hid_state.gatt_handle,
                        hid_state.hid_service_start, hid_state.hid_service_end,
                        0x2A4D);  // HID Report UUID
                } else {
                    printf("[BTSTACK_HOST] No HID service found!\n");
                }
            } else if (hid_state.gatt_state == GATT_DISCOVERING_HID_CHARACTERISTICS) {
                if (hid_state.report_characteristic.value_handle != 0) {
                    // Found Report char, enable notifications
                    printf("[BTSTACK_HOST] Enabling notifications on 0x%04X (end=0x%04X)...\n",
                           hid_state.report_characteristic.value_handle,
                           hid_state.report_characteristic.end_handle);
                    hid_state.gatt_state = GATT_ENABLING_NOTIFICATIONS;
                    gatt_client_write_client_characteristic_configuration(
                        gatt_client_callback, hid_state.gatt_handle,
                        &hid_state.report_characteristic,
                        GATT_CLIENT_CHARACTERISTICS_CONFIGURATION_NOTIFICATION);
                } else {
                    printf("[BTSTACK_HOST] No HID Report characteristic found!\n");
                }
            } else if (hid_state.gatt_state == GATT_ENABLING_NOTIFICATIONS) {
                printf("[BTSTACK_HOST] Notifications enabled! Ready for HID reports.\n");
                hid_state.gatt_state = GATT_READY;
            }
            break;
        }

        case GATT_EVENT_NOTIFICATION: {
            hci_con_handle_t con_handle = gatt_event_notification_get_handle(packet);
            uint16_t value_handle = gatt_event_notification_get_value_handle(packet);
            uint16_t value_length = gatt_event_notification_get_value_length(packet);
            const uint8_t *value = gatt_event_notification_get_value(packet);

            // BLE HID Report characteristic (Xbox uses handle 0x001E)
            // Route through bthid layer
            if (value_handle == 0x001E && value_length >= 1) {
                int conn_index = get_ble_conn_index_by_handle(con_handle);
                if (conn_index >= 0) {
                    route_ble_hid_report(conn_index, value, value_length);
                }
            }
            break;
        }
    }
}

// ============================================================================
// DIRECT XBOX HID NOTIFICATION HANDLER
// ============================================================================

// Handle notifications directly from gatt_client listener API
static void ble_hid_notification_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size)
{
    UNUSED(channel);
    UNUSED(size);

    if (packet_type != HCI_EVENT_PACKET) return;
    if (hci_event_packet_get_type(packet) != GATT_EVENT_NOTIFICATION) return;

    hci_con_handle_t con_handle = gatt_event_notification_get_handle(packet);
    uint16_t value_handle = gatt_event_notification_get_value_handle(packet);
    uint16_t value_length = gatt_event_notification_get_value_length(packet);
    const uint8_t *value = gatt_event_notification_get_value(packet);

    // Debug: log all notifications to identify chatpad/keyboard reports
    static uint16_t last_handle = 0;
    static uint16_t last_len = 0;
    if (value_handle != last_handle || value_length != last_len) {
        printf("[BTSTACK_HOST] BLE notif: handle=0x%04X len=%d data=%02X %02X %02X %02X\n",
               value_handle, value_length,
               value_length > 0 ? value[0] : 0,
               value_length > 1 ? value[1] : 0,
               value_length > 2 ? value[2] : 0,
               value_length > 3 ? value[3] : 0);
        last_handle = value_handle;
        last_len = value_length;
    }

    // Accept HID report notifications - filter by reasonable gamepad report length
    if (value_length < 10 || value_length > sizeof(pending_ble_report)) return;

    // Get conn_index for this BLE connection
    int conn_index = get_ble_conn_index_by_handle(con_handle);
    if (conn_index < 0) return;

    // Defer processing to main loop to avoid stack overflow
    memcpy(pending_ble_report, value, value_length);
    pending_ble_report_len = value_length;
    pending_ble_conn_index = (uint8_t)conn_index;
    ble_report_pending = true;
}

// Register direct listener for BLE HID notifications and notify bthid layer
static void register_ble_hid_listener(hci_con_handle_t con_handle)
{
    printf("[BTSTACK_HOST] Registering BLE HID listener for handle 0x%04X\n", con_handle);

    // Find the BLE connection
    ble_connection_t* conn = find_connection_by_handle(con_handle);
    if (!conn) {
        printf("[BTSTACK_HOST] ERROR: No connection for handle 0x%04X\n", con_handle);
        return;
    }

    // Assign conn_index if not already set
    int ble_index = -1;
    for (int i = 0; i < MAX_BLE_CONNECTIONS; i++) {
        if (&hid_state.connections[i] == conn) {
            ble_index = i;
            break;
        }
    }
    if (ble_index < 0) return;

    conn->conn_index = BLE_CONN_INDEX_OFFSET + ble_index;
    conn->hid_ready = true;

    // Set up a fake characteristic structure with just the value_handle
    // Xbox BLE HID Report characteristic value handle is 0x001E
    memset(&xbox_hid_characteristic, 0, sizeof(xbox_hid_characteristic));
    xbox_hid_characteristic.value_handle = 0x001E;
    xbox_hid_characteristic.end_handle = 0x001F;  // Approximate

    // Register to listen for notifications on the HID report characteristic
    gatt_client_listen_for_characteristic_value_updates(
        &xbox_hid_notification_listener,
        ble_hid_notification_handler,
        con_handle,
        &xbox_hid_characteristic);

    printf("[BTSTACK_HOST] BLE HID listener registered, conn_index=%d\n", conn->conn_index);

    // Notify bthid layer that device is ready
    btstack_host_stop_scan();
    scan_timeout_end = 0;
    printf("[BTSTACK_HOST] Calling bt_on_hid_ready(%d) for BLE device '%s'\n", conn->conn_index, conn->name);
    bt_on_hid_ready(conn->conn_index);
}

// ============================================================================
// SWITCH 2 BLE HID NOTIFICATION HANDLER
// ============================================================================

// Switch 2 ATT handles (from protocol documentation)
#define SW2_INPUT_REPORT_HANDLE     0x000A  // Input reports via notification
#define SW2_CCC_HANDLE              0x000B  // Client Characteristic Configuration
#define SW2_OUTPUT_REPORT_HANDLE    0x0012  // Rumble output
#define SW2_CMD_HANDLE              0x0014  // Command output
#define SW2_ACK_CCC_HANDLE          0x001B  // ACK notification CCC

// Switch 2 command constants
#define SW2_CMD_PAIRING             0x15
#define SW2_CMD_SET_LED             0x09
#define SW2_CMD_READ_SPI            0x02
#define SW2_REQ_TYPE_REQ            0x91
#define SW2_REQ_INT_BLE             0x01
#define SW2_SUBCMD_SET_LED          0x07
#define SW2_SUBCMD_READ_SPI         0x04
// Pairing subcmds - sent in order: STEP1 -> STEP2 -> STEP3 -> STEP4
// Note: Response ACK contains same subcmd as request
#define SW2_SUBCMD_PAIRING_STEP1    0x01  // Send BD address
#define SW2_SUBCMD_PAIRING_STEP2    0x04  // Send magic bytes 1
#define SW2_SUBCMD_PAIRING_STEP3    0x02  // Send magic bytes 2
#define SW2_SUBCMD_PAIRING_STEP4    0x03  // Complete pairing

// Init state machine states (matching BlueRetro's sequence)
typedef enum {
    SW2_INIT_IDLE = 0,
    SW2_INIT_READ_INFO,             // Read device info from SPI
    SW2_INIT_READ_LTK,              // Read LTK to check if paired
    SW2_INIT_PAIR_STEP1,            // Pairing step 1 (BD addr)
    SW2_INIT_PAIR_STEP2,            // Pairing step 2
    SW2_INIT_PAIR_STEP3,            // Pairing step 3
    SW2_INIT_PAIR_STEP4,            // Pairing step 4
    SW2_INIT_SET_LED,               // Set player LED
    SW2_INIT_DONE                   // Init complete
} sw2_init_state_t;

// Handle Switch 2 HID notifications
static void switch2_hid_notification_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size)
{
    UNUSED(channel);
    UNUSED(size);

    if (packet_type != HCI_EVENT_PACKET) return;
    if (hci_event_packet_get_type(packet) != GATT_EVENT_NOTIFICATION) return;

    hci_con_handle_t con_handle = gatt_event_notification_get_handle(packet);
    uint16_t value_handle = gatt_event_notification_get_value_handle(packet);
    uint16_t value_length = gatt_event_notification_get_value_length(packet);
    const uint8_t *value = gatt_event_notification_get_value(packet);

    // Debug first notification
    static bool sw2_notif_debug = false;
    if (!sw2_notif_debug) {
        printf("[SW2_BLE] Notification: handle=0x%04X len=%d data=%02X %02X %02X %02X\n",
               value_handle, value_length,
               value_length > 0 ? value[0] : 0,
               value_length > 1 ? value[1] : 0,
               value_length > 2 ? value[2] : 0,
               value_length > 3 ? value[3] : 0);
        sw2_notif_debug = true;
    }

    // Switch 2 input reports are 64 bytes on handle 0x000A
    if (value_handle != SW2_INPUT_REPORT_HANDLE) return;
    if (value_length < 16 || value_length > sizeof(pending_ble_report)) return;

    // Get conn_index for this BLE connection
    int conn_index = get_ble_conn_index_by_handle(con_handle);
    if (conn_index < 0) return;

    // Defer processing to main loop to avoid stack overflow
    memcpy(pending_ble_report, value, value_length);
    pending_ble_report_len = value_length;
    pending_ble_conn_index = (uint8_t)conn_index;
    ble_report_pending = true;
}

// Forward declarations for Switch 2
static void switch2_send_next_init_cmd(hci_con_handle_t con_handle);

// CCC write completion handler for Switch 2 input reports
static void switch2_ccc_write_callback(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size)
{
    UNUSED(channel);
    UNUSED(size);

    if (packet_type != HCI_EVENT_PACKET) return;
    if (hci_event_packet_get_type(packet) != GATT_EVENT_QUERY_COMPLETE) return;

    uint8_t status = gatt_event_query_complete_get_att_status(packet);
    hci_con_handle_t handle = gatt_event_query_complete_get_handle(packet);

    if (status == ATT_ERROR_SUCCESS) {
        printf("[SW2_BLE] Input notifications enabled for handle 0x%04X\n", handle);

        // Now register the notification listener
        ble_connection_t* conn = find_connection_by_handle(handle);
        if (conn) {
            // Update bthid with VID/PID BEFORE calling bt_on_hid_ready
            // so driver selection has correct info
            printf("[SW2_BLE] Updating device info: VID=0x%04X PID=0x%04X\n", conn->vid, conn->pid);
            bthid_update_device_info(conn->conn_index, conn->name, conn->vid, conn->pid);

            // Notify bthid layer that device is ready
            btstack_host_stop_scan();
            scan_timeout_end = 0;
            printf("[SW2_BLE] Calling bt_on_hid_ready(%d) for Switch 2 device\n", conn->conn_index);
            bt_on_hid_ready(conn->conn_index);
        }
    } else {
        printf("[SW2_BLE] Failed to enable input notifications: status=0x%02X\n", status);
    }
}

// CCC write completion handler for Switch 2 ACK notifications
static void switch2_ack_ccc_write_callback(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size)
{
    UNUSED(channel);
    UNUSED(size);

    if (packet_type != HCI_EVENT_PACKET) return;
    if (hci_event_packet_get_type(packet) != GATT_EVENT_QUERY_COMPLETE) return;

    uint8_t status = gatt_event_query_complete_get_att_status(packet);
    hci_con_handle_t handle = gatt_event_query_complete_get_handle(packet);

    if (status == ATT_ERROR_SUCCESS) {
        printf("[SW2_BLE] ACK notifications enabled for handle 0x%04X\n", handle);

        // Now enable input report notifications
        static uint8_t ccc_enable[] = { 0x01, 0x00 };
        printf("[SW2_BLE] Enabling input notifications on CCC handle 0x%04X\n", SW2_CCC_HANDLE);
        gatt_client_write_value_of_characteristic(
            switch2_ccc_write_callback, handle, SW2_CCC_HANDLE, sizeof(ccc_enable), ccc_enable);

        // Start the pairing sequence
        printf("[SW2_BLE] Starting pairing sequence\n");
        switch2_send_next_init_cmd(handle);
    } else {
        printf("[SW2_BLE] Failed to enable ACK notifications: status=0x%02X\n", status);
    }
}

// Switch 2 init state machine
static sw2_init_state_t sw2_init_state = SW2_INIT_IDLE;
static hci_con_handle_t sw2_init_handle = 0;

// ACK notification listener for Switch 2 commands
static gatt_client_notification_t switch2_ack_notification_listener;
static gatt_client_characteristic_t switch2_ack_characteristic;

// Cleanup Switch 2 state on BLE disconnect (called from disconnect handler)
static void switch2_cleanup_on_disconnect(void) {
    gatt_client_stop_listening_for_characteristic_value_updates(&switch2_ack_notification_listener);
    sw2_init_state = SW2_INIT_IDLE;
    sw2_init_handle = 0;
}

// Forward declare
static void switch2_send_init_cmd(hci_con_handle_t con_handle);

static void switch2_ack_notification_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size)
{
    UNUSED(channel);
    UNUSED(size);

    if (packet_type != HCI_EVENT_PACKET) return;
    if (hci_event_packet_get_type(packet) != GATT_EVENT_NOTIFICATION) return;

    uint16_t value_handle = gatt_event_notification_get_value_handle(packet);
    uint16_t value_length = gatt_event_notification_get_value_length(packet);
    const uint8_t *value = gatt_event_notification_get_value(packet);
    hci_con_handle_t con_handle = gatt_event_notification_get_handle(packet);

    // Debug: print all notifications (not just 0x001A) to see what's coming in
    static bool ack_notif_debug = false;
    if (!ack_notif_debug && value_handle != SW2_INPUT_REPORT_HANDLE) {
        printf("[SW2_BLE] ACK listener got notification: handle=0x%04X len=%d\n",
               value_handle, value_length);
        ack_notif_debug = true;
    }

    if (value_handle != 0x001A) return;  // ACK handle

    if (value_length < 4) return;
    uint8_t cmd = value[0];
    uint8_t subcmd = value[3];

    printf("[SW2_BLE] ACK: cmd=0x%02X subcmd=0x%02X state=%d len=%d\n",
           cmd, subcmd, sw2_init_state, value_length);

    // Handle ACK based on current init state
    switch (cmd) {
        case SW2_CMD_READ_SPI:
            if (sw2_init_state == SW2_INIT_READ_INFO) {
                // Got device info, extract VID/PID if needed
                if (value_length >= 34) {
                    uint16_t vid = value[30] | (value[31] << 8);
                    uint16_t pid = value[32] | (value[33] << 8);
                    printf("[SW2_BLE] Device info: VID=0x%04X PID=0x%04X\n", vid, pid);
                }
                // Skip LTK check for now, go straight to pairing
                sw2_init_state = SW2_INIT_PAIR_STEP1;
                switch2_send_init_cmd(con_handle);
            } else if (sw2_init_state == SW2_INIT_READ_LTK) {
                // Check LTK, for now just proceed to pairing
                sw2_init_state = SW2_INIT_PAIR_STEP1;
                switch2_send_init_cmd(con_handle);
            }
            break;

        case SW2_CMD_PAIRING:
            switch (subcmd) {
                case SW2_SUBCMD_PAIRING_STEP1:
                    if (sw2_init_state == SW2_INIT_PAIR_STEP1) {
                        sw2_init_state = SW2_INIT_PAIR_STEP2;
                        switch2_send_init_cmd(con_handle);
                    }
                    break;
                case SW2_SUBCMD_PAIRING_STEP2:
                    if (sw2_init_state == SW2_INIT_PAIR_STEP2) {
                        sw2_init_state = SW2_INIT_PAIR_STEP3;
                        switch2_send_init_cmd(con_handle);
                    }
                    break;
                case SW2_SUBCMD_PAIRING_STEP3:
                    if (sw2_init_state == SW2_INIT_PAIR_STEP3) {
                        sw2_init_state = SW2_INIT_PAIR_STEP4;
                        switch2_send_init_cmd(con_handle);
                    }
                    break;
                case SW2_SUBCMD_PAIRING_STEP4:
                    if (sw2_init_state == SW2_INIT_PAIR_STEP4) {
                        printf("[SW2_BLE] Pairing complete! Setting LED...\n");
                        sw2_init_state = SW2_INIT_SET_LED;
                        switch2_send_init_cmd(con_handle);
                    }
                    break;
            }
            break;

        case SW2_CMD_SET_LED:
            if (sw2_init_state == SW2_INIT_SET_LED) {
                printf("[SW2_BLE] LED set! Init done.\n");
                sw2_init_state = SW2_INIT_DONE;
            }
            break;
    }
}

static void switch2_send_init_cmd(hci_con_handle_t con_handle)
{
    printf("[SW2_BLE] Sending init cmd, state=%d\n", sw2_init_state);

    switch (sw2_init_state) {
        case SW2_INIT_READ_INFO: {
            // Read device info from SPI (BlueRetro's first step)
            uint8_t read_info[] = {
                SW2_CMD_READ_SPI,       // 0x02
                SW2_REQ_TYPE_REQ,       // 0x91
                SW2_REQ_INT_BLE,        // 0x01
                SW2_SUBCMD_READ_SPI,    // 0x04
                0x00, 0x08, 0x00, 0x00,
                0x40,                   // Read length
                0x7e, 0x00, 0x00,       // Address type
                0x00, 0x30, 0x01, 0x00  // SPI address
            };
            gatt_client_write_value_of_characteristic_without_response(
                con_handle, SW2_CMD_HANDLE, sizeof(read_info), read_info);
            printf("[SW2_BLE] READ_INFO sent\n");
            break;
        }

        case SW2_INIT_PAIR_STEP1: {
            // Pairing step 1: Send our BD address
            bd_addr_t local_addr;
            gap_local_bd_addr(local_addr);
            printf("[SW2_BLE] Pair Step 1: BD addr = %02X:%02X:%02X:%02X:%02X:%02X\n",
                   local_addr[5], local_addr[4], local_addr[3],
                   local_addr[2], local_addr[1], local_addr[0]);

            uint8_t pair1[] = {
                SW2_CMD_PAIRING,        // 0x15
                SW2_REQ_TYPE_REQ,       // 0x91
                SW2_REQ_INT_BLE,        // 0x01
                SW2_SUBCMD_PAIRING_STEP1, // 0x01
                0x00, 0x0e, 0x00, 0x00, 0x00, 0x02,
                // 6 bytes: our BD addr
                local_addr[0], local_addr[1], local_addr[2],
                local_addr[3], local_addr[4], local_addr[5],
                // 6 bytes: our BD addr - 1
                (uint8_t)(local_addr[0] - 1), local_addr[1], local_addr[2],
                local_addr[3], local_addr[4], local_addr[5],
            };
            gatt_client_write_value_of_characteristic_without_response(
                con_handle, SW2_CMD_HANDLE, sizeof(pair1), pair1);
            break;
        }

        case SW2_INIT_PAIR_STEP2: {
            // Pairing step 2: Magic bytes (from BlueRetro)
            uint8_t pair2[] = {
                SW2_CMD_PAIRING,        // 0x15
                SW2_REQ_TYPE_REQ,       // 0x91
                SW2_REQ_INT_BLE,        // 0x01
                SW2_SUBCMD_PAIRING_STEP2, // 0x04
                0x00, 0x11, 0x00, 0x00, 0x00,
                0xea, 0xbd, 0x47, 0x13, 0x89, 0x35, 0x42, 0xc6,
                0x79, 0xee, 0x07, 0xf2, 0x53, 0x2c, 0x6c, 0x31
            };
            gatt_client_write_value_of_characteristic_without_response(
                con_handle, SW2_CMD_HANDLE, sizeof(pair2), pair2);
            printf("[SW2_BLE] Pair Step 2 sent\n");
            break;
        }

        case SW2_INIT_PAIR_STEP3: {
            // Pairing step 3: More magic bytes
            uint8_t pair3[] = {
                SW2_CMD_PAIRING,        // 0x15
                SW2_REQ_TYPE_REQ,       // 0x91
                SW2_REQ_INT_BLE,        // 0x01
                SW2_SUBCMD_PAIRING_STEP3, // 0x02
                0x00, 0x11, 0x00, 0x00, 0x00,
                0x40, 0xb0, 0x8a, 0x5f, 0xcd, 0x1f, 0x9b, 0x41,
                0x12, 0x5c, 0xac, 0xc6, 0x3f, 0x38, 0xa0, 0x73
            };
            gatt_client_write_value_of_characteristic_without_response(
                con_handle, SW2_CMD_HANDLE, sizeof(pair3), pair3);
            printf("[SW2_BLE] Pair Step 3 sent\n");
            break;
        }

        case SW2_INIT_PAIR_STEP4: {
            // Pairing step 4: Completion
            uint8_t pair4[] = {
                SW2_CMD_PAIRING,        // 0x15
                SW2_REQ_TYPE_REQ,       // 0x91
                SW2_REQ_INT_BLE,        // 0x01
                SW2_SUBCMD_PAIRING_STEP4, // 0x03
                0x00, 0x01, 0x00, 0x00, 0x00
            };
            gatt_client_write_value_of_characteristic_without_response(
                con_handle, SW2_CMD_HANDLE, sizeof(pair4), pair4);
            printf("[SW2_BLE] Pair Step 4 sent\n");
            break;
        }

        case SW2_INIT_SET_LED: {
            // Set player LED
            uint8_t led_cmd[] = {
                SW2_CMD_SET_LED,        // 0x09
                SW2_REQ_TYPE_REQ,       // 0x91
                SW2_REQ_INT_BLE,        // 0x01
                SW2_SUBCMD_SET_LED,     // 0x07
                0x00, 0x08, 0x00, 0x00,
                0x01,  // Player 1 LED pattern
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            };
            gatt_client_write_value_of_characteristic_without_response(
                con_handle, SW2_CMD_HANDLE, sizeof(led_cmd), led_cmd);
            printf("[SW2_BLE] LED command sent\n");
            break;
        }

        default:
            printf("[SW2_BLE] Unknown init state: %d\n", sw2_init_state);
            break;
    }
}

static void switch2_send_next_init_cmd(hci_con_handle_t con_handle)
{
    // Start the init sequence with READ_INFO (like BlueRetro does)
    if (sw2_init_state == SW2_INIT_IDLE) {
        printf("[SW2_BLE] Starting init sequence with READ_INFO...\n");
        sw2_init_state = SW2_INIT_READ_INFO;
        switch2_send_init_cmd(con_handle);
    } else if (sw2_init_state == SW2_INIT_DONE) {
        printf("[SW2_BLE] Init already done\n");
    } else {
        // Init in progress, wait for ACK
        printf("[SW2_BLE] Init in progress (state=%d)\n", sw2_init_state);
    }
}

// Retry init if stuck (called from main loop)
static void switch2_retry_init_if_needed(void)
{
    static uint32_t retry_counter = 0;
    retry_counter++;

    if (sw2_init_state != SW2_INIT_IDLE && sw2_init_state != SW2_INIT_DONE && sw2_init_handle != 0) {
        // Retry every ~500ms (assuming ~120Hz main loop = 60 counts)
        if (retry_counter % 60 == 0) {
            printf("[SW2_BLE] Retrying init cmd (state=%d, attempt=%lu)\n",
                   sw2_init_state, (unsigned long)(retry_counter / 60));
            switch2_send_init_cmd(sw2_init_handle);
        }
    }
}

// ============================================================================
// SWITCH 2 RUMBLE/HAPTICS
// ============================================================================
// Switch 2 Pro Controller uses LRA (Linear Resonant Actuator) haptics.
// Output goes to ATT handle 0x0012.
// LRA ops format: 5 bytes per op (4-byte bitfield + 1-byte hf_amp)
// Each side (L/R) has 1 state byte + 3 ops = 16 bytes
// Total output: 1 + 16 + 16 + 9 padding = 42 bytes

// Rumble state tracking
static uint8_t sw2_last_rumble_left = 0;
static uint8_t sw2_last_rumble_right = 0;
static uint8_t sw2_rumble_tid = 0;
static uint32_t sw2_rumble_send_counter = 0;

// Player LED state tracking
static uint8_t sw2_last_player_led = 0;

// Player LED patterns (cumulative, matching joypad-web)
static const uint8_t SW2_PLAYER_LED_PATTERNS[] = {
    0x01,  // Player 1: 1 LED
    0x03,  // Player 2: 2 LEDs
    0x07,  // Player 3: 3 LEDs
    0x0F,  // Player 4: 4 LEDs
};

// Send player LED command to Switch 2 controller
static void switch2_send_player_led(hci_con_handle_t con_handle, uint8_t pattern)
{
    uint8_t led_cmd[] = {
        SW2_CMD_SET_LED,        // 0x09
        SW2_REQ_TYPE_REQ,       // 0x91
        SW2_REQ_INT_BLE,        // 0x01
        SW2_SUBCMD_SET_LED,     // 0x07
        0x00, 0x08, 0x00, 0x00,
        pattern,  // Player LED pattern
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    gatt_client_write_value_of_characteristic_without_response(
        con_handle, SW2_CMD_HANDLE, sizeof(led_cmd), led_cmd);
}

// Encode haptic data for one motor (5 bytes)
// Based on joypad-web's encodeSwitch2Haptic() function
// Format: [amplitude, frequency, amplitude, frequency, flags]
// Key: Lower frequency = more felt, higher frequency = audible tones
// freq 0x60 = felt rumble, freq 0xFE = audible (avoid this)
static void encode_haptic(uint8_t* out, uint8_t intensity)
{
    if (intensity == 0) {
        // Off state
        out[0] = 0x00;
        out[1] = 0x00;
        out[2] = 0x00;
        out[3] = 0x00;
        out[4] = 0x00;
    } else {
        // Active rumble - use low frequency for felt vibration
        // Amplitude: scale from 0x40 to 0xFF based on intensity
        uint8_t amp = 0x40 + ((intensity * 0xBF) / 255);
        // Frequency: use 0x40-0x60 range for low rumble (more felt, less audible)
        // Lower values = lower frequency = more physical sensation
        uint8_t freq = 0x40;  // Low frequency for maximum felt rumble
        out[0] = amp;   // High band amplitude
        out[1] = freq;  // High band frequency (low value = felt)
        out[2] = amp;   // Low band amplitude
        out[3] = freq;  // Low band frequency
        out[4] = 0x00;  // Flags
    }
}

// Send rumble command to Switch 2 controller via BLE
// Pro uses HD haptics, GameCube uses simple on/off
static void switch2_send_rumble(hci_con_handle_t con_handle, uint8_t left, uint8_t right)
{
    // Get connection to check PID
    ble_connection_t* conn = find_connection_by_handle(con_handle);

    uint8_t buf[64];
    memset(buf, 0, sizeof(buf));

    // Counter with state bits
    uint8_t counter = 0x50 | (sw2_rumble_tid & 0x0F);
    sw2_rumble_tid++;

    if (conn && conn->pid == 0x2073) {
        // GameCube controller: simple on/off rumble
        // Format: byte 1 = counter, byte 2 = on/off state
        buf[1] = counter;
        buf[2] = (left || right) ? 0x01 : 0x00;

        gatt_client_write_value_of_characteristic_without_response(
            con_handle, SW2_OUTPUT_REPORT_HANDLE, 21, buf);
    } else {
        // Pro controller: HD haptics format
        // [1]: Counter (0x5X)
        // [2-6]: Left haptic (5 bytes)
        // [17]: Counter duplicate
        // [18-22]: Right haptic (5 bytes)
        buf[1] = counter;
        buf[17] = counter;  // Duplicate counter

        // Encode left motor haptic (bytes 2-6)
        encode_haptic(&buf[2], left);

        // Encode right motor haptic (bytes 18-22)
        encode_haptic(&buf[18], right);

        gatt_client_write_value_of_characteristic_without_response(
            con_handle, SW2_OUTPUT_REPORT_HANDLE, sizeof(buf), buf);
    }
}

// Check feedback system and send rumble/LED if needed (called from task loop)
static void switch2_handle_feedback(void)
{
    // Only process if we have an active Switch 2 connection
    if (sw2_init_state != SW2_INIT_DONE || sw2_init_handle == 0) return;

    sw2_rumble_send_counter++;

    // Get conn_index from HCI handle
    int conn_index = get_ble_conn_index_by_handle(sw2_init_handle);
    if (conn_index < 0) return;

    // Find player index for this device
    int player_idx = find_player_index(conn_index, 0);
    if (player_idx < 0) return;

    // Get feedback state
    feedback_state_t* fb = feedback_get_state(player_idx);
    if (!fb) return;

    // --- Handle Player LED ---
    if (fb->led_dirty) {
        // Determine LED pattern from feedback
        uint8_t led_pattern = 0x01;  // Default to player 1

        if (fb->led.pattern != 0) {
            // Use pattern bits directly (0x01=P1, 0x02=P2, 0x04=P3, 0x08=P4)
            // Convert to cumulative pattern for Switch 2
            if (fb->led.pattern & 0x08) led_pattern = SW2_PLAYER_LED_PATTERNS[3];
            else if (fb->led.pattern & 0x04) led_pattern = SW2_PLAYER_LED_PATTERNS[2];
            else if (fb->led.pattern & 0x02) led_pattern = SW2_PLAYER_LED_PATTERNS[1];
            else led_pattern = SW2_PLAYER_LED_PATTERNS[0];
        } else {
            // Use player index if no explicit pattern
            int idx = (player_idx >= 0 && player_idx < 4) ? player_idx : 0;
            led_pattern = SW2_PLAYER_LED_PATTERNS[idx];
        }

        if (led_pattern != sw2_last_player_led) {
            sw2_last_player_led = led_pattern;
            switch2_send_player_led(sw2_init_handle, led_pattern);
        }
    }

    // --- Handle Rumble ---
    bool value_changed = (fb->rumble.left != sw2_last_rumble_left ||
                          fb->rumble.right != sw2_last_rumble_right);

    // Send rumble if:
    // 1. Values changed, OR
    // 2. Rumble is active and we need periodic refresh (every ~50ms at 120Hz = 6 ticks)
    bool need_refresh = (sw2_last_rumble_left > 0 || sw2_last_rumble_right > 0) &&
                        (sw2_rumble_send_counter % 6 == 0);

    if (fb->rumble_dirty || value_changed || need_refresh) {
        sw2_last_rumble_left = fb->rumble.left;
        sw2_last_rumble_right = fb->rumble.right;

        switch2_send_rumble(sw2_init_handle, fb->rumble.left, fb->rumble.right);
    }

    // Clear dirty flags after processing
    if (fb->rumble_dirty || fb->led_dirty) {
        feedback_clear_dirty(player_idx);
    }
}

// Register Switch 2 notification listener and enable notifications
static void register_switch2_hid_listener(hci_con_handle_t con_handle)
{
    printf("[SW2_BLE] Registering Switch 2 HID listener for handle 0x%04X\n", con_handle);

    // Find the BLE connection
    ble_connection_t* conn = find_connection_by_handle(con_handle);
    if (!conn) {
        printf("[SW2_BLE] ERROR: No connection for handle 0x%04X\n", con_handle);
        return;
    }

    // Assign conn_index if not already set
    int ble_index = -1;
    for (int i = 0; i < MAX_BLE_CONNECTIONS; i++) {
        if (&hid_state.connections[i] == conn) {
            ble_index = i;
            break;
        }
    }
    if (ble_index < 0) return;

    conn->conn_index = BLE_CONN_INDEX_OFFSET + ble_index;
    conn->hid_ready = true;
    sw2_init_handle = con_handle;
    sw2_init_state = SW2_INIT_IDLE;

    printf("[SW2_BLE] Connection: VID=0x%04X PID=0x%04X conn_index=%d\n",
           conn->vid, conn->pid, conn->conn_index);

    // Set up ACK notification listener (handle 0x001A)
    memset(&switch2_ack_characteristic, 0, sizeof(switch2_ack_characteristic));
    switch2_ack_characteristic.value_handle = 0x001A;
    switch2_ack_characteristic.end_handle = 0x001A + 1;

    gatt_client_listen_for_characteristic_value_updates(
        &switch2_ack_notification_listener,
        switch2_ack_notification_handler,
        con_handle,
        &switch2_ack_characteristic);

    // Set up input report notification listener (handle 0x000A)
    memset(&switch2_hid_characteristic, 0, sizeof(switch2_hid_characteristic));
    switch2_hid_characteristic.value_handle = SW2_INPUT_REPORT_HANDLE;
    switch2_hid_characteristic.end_handle = SW2_INPUT_REPORT_HANDLE + 1;

    gatt_client_listen_for_characteristic_value_updates(
        &switch2_hid_notification_listener,
        switch2_hid_notification_handler,
        con_handle,
        &switch2_hid_characteristic);

    printf("[SW2_BLE] Notification listeners registered\n");

    // Enable notifications on ACK handle first (0x001B) - wait for confirmation
    static uint8_t ccc_enable[] = { 0x01, 0x00 };
    printf("[SW2_BLE] Enabling ACK notifications on CCC handle 0x%04X\n", SW2_ACK_CCC_HANDLE);
    gatt_client_write_value_of_characteristic(
        switch2_ack_ccc_write_callback, con_handle, SW2_ACK_CCC_HANDLE, sizeof(ccc_enable), ccc_enable);
}

static void start_hids_client(ble_connection_t *conn)
{
    printf("[BTSTACK_HOST] Connecting HIDS client...\n");

    conn->state = BLE_STATE_DISCOVERING;
    hid_state.gatt_handle = conn->handle;

    uint8_t status = hids_client_connect(conn->handle, hids_client_handler,
                                         HID_PROTOCOL_MODE_REPORT, &conn->hids_cid);

    printf("[BTSTACK_HOST] hids_client_connect returned %d, cid=0x%04X\n",
           status, conn->hids_cid);
}

// ============================================================================
// BAS (BATTERY SERVICE) CLIENT HANDLER
// ============================================================================

static void bas_client_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size)
{
    UNUSED(packet_type);
    UNUSED(channel);
    UNUSED(size);

    if (hci_event_packet_get_type(packet) != HCI_EVENT_GATTSERVICE_META) return;

    switch (hci_event_gattservice_meta_get_subevent_code(packet)) {
        case GATTSERVICE_SUBEVENT_BATTERY_SERVICE_CONNECTED: {
            uint8_t status = gattservice_subevent_battery_service_connected_get_status(packet);
            uint8_t num_instances = gattservice_subevent_battery_service_connected_get_num_instances(packet);
            printf("[BTSTACK_HOST] BAS connected: status=%d instances=%d\n", status, num_instances);
            break;
        }

        case GATTSERVICE_SUBEVENT_BATTERY_SERVICE_LEVEL: {
            uint8_t att_status = gattservice_subevent_battery_service_level_get_att_status(packet);
            uint8_t level = gattservice_subevent_battery_service_level_get_level(packet);

            if (att_status != ATT_ERROR_SUCCESS) break;

            // Find conn_index for the current BLE connection
            int conn_index = get_ble_conn_index_by_handle(hid_state.gatt_handle);
            if (conn_index >= 0) {
                bthid_set_battery_level((uint8_t)conn_index, level);
            }
            // Cache for the MouthPad relay's connection-status response.
            if (hid_state.gatt_handle == mp_nus.handle) {
                mp_nus.last_battery = level;
            }
            break;
        }

        default:
            break;
    }
}

static void start_battery_service_client(hci_con_handle_t handle)
{
    uint8_t status = battery_service_client_connect(handle, bas_client_handler, 60000, &hid_state.bas_cid);
    if (status != ERROR_CODE_SUCCESS) {
        printf("[BTSTACK_HOST] BAS connect failed: status=%d\n", status);
    } else {
        printf("[BTSTACK_HOST] BAS connect started: cid=0x%04X\n", hid_state.bas_cid);
    }
}

// ============================================================================
// DIS (DEVICE INFORMATION SERVICE) CLIENT HANDLER
// ============================================================================

static void dis_client_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size)
{
    UNUSED(packet_type);
    UNUSED(channel);
    UNUSED(size);

    if (hci_event_packet_get_type(packet) != HCI_EVENT_GATTSERVICE_META) return;

    switch (hci_event_gattservice_meta_get_subevent_code(packet)) {
        case GATTSERVICE_SUBEVENT_DEVICE_INFORMATION_PNP_ID: {
            hci_con_handle_t handle = gattservice_subevent_device_information_pnp_id_get_con_handle(packet);
            uint16_t vid = gattservice_subevent_device_information_pnp_id_get_vendor_id(packet);
            uint16_t pid = gattservice_subevent_device_information_pnp_id_get_product_id(packet);
            uint8_t vendor_source = gattservice_subevent_device_information_pnp_id_get_vendor_source_id(packet);

            printf("[BTSTACK_HOST] DIS PnP ID: vendor_source=%d VID=0x%04X PID=0x%04X handle=0x%04X\n",
                   vendor_source, vid, pid, handle);

            ble_connection_t *conn = find_connection_by_handle(handle);
            if (conn && (conn->vid != vid || conn->pid != pid)) {
                conn->vid = vid;
                conn->pid = pid;
                printf("[BTSTACK_HOST] DIS: updating device info for conn_index=%d\n", conn->conn_index);
                bthid_update_device_info(conn->conn_index, conn->name, vid, pid);
            }

            // Steam Controller 2 reconnect fix: on a bonded reconnect the SC2 advertises
            // with NO name, so bt_device_lookup ("Steam" substring) misses and it comes
            // up on the generic HOGP/HIDS path — which leaves it in lizard mode with no
            // gamepad reports (buttons dead until a fresh pair). DIS PnP ID is the first
            // point we can positively identify it (VID 0x28DE / PID 0x1303), so switch it
            // onto the Valve GATT client here: drop the generic HIDS client and start the
            // Valve discovery (lizard-off + input 0x45 + rumble + battery + trackpad).
            if (conn && vid == 0x28DE &&
                !(conn->profile && conn->profile->ble == BT_BLE_VALVE)) {
                printf("[SC2_BLE] DIS identified SC2 on generic path — switching to Valve GATT client\n");
                conn->profile = &BT_PROFILE_STEAM_CONTROLLER2;
                if (conn->hids_cid) {
                    hids_client_disconnect(conn->hids_cid);
                    conn->hids_cid = 0;
                }
                register_valve_hid_listener(handle);
                break;  // don't fall through to NUS matching
            }
            // Recognize NUS peers by DIS PnP ID and arm the NUS client — names
            // can be reset to dev values that miss the name gate at the 0x1C
            // handler (which leaves the relay stuck "scanning").
            // mp_nus_mark_pending is a no-op if already armed by the name gate.
            //   0x1915:0xEEEE — Augmental MouthPad
            //   0x2E8A:0x10C6 — JoypadOS BLE controller (face relay)
            if ((vid == 0x1915 && pid == 0xEEEE) ||
                (vid == 0x2E8A && pid == 0x10C6)) {
                mp_nus_mark_pending(handle);
            }
            break;
        }

        case GATTSERVICE_SUBEVENT_DEVICE_INFORMATION_FIRMWARE_REVISION: {
            hci_con_handle_t handle = gattservice_subevent_device_information_firmware_revision_get_con_handle(packet);
            // Store for the active DIS connection, NOT gated on mp_nus being armed:
            // PnP ID (which arms mp_nus by VID/PID) comes LAST in DIS, after the
            // firmware-revision read — gating on mp_nus.handle here would miss it.
            if (gattservice_subevent_device_information_firmware_revision_get_att_status(packet) == ATT_ERROR_SUCCESS
                && handle == hid_state.gatt_handle) {
                const char* fw = gattservice_subevent_device_information_firmware_revision_get_value(packet);
                if (fw) {
                    strncpy(mp_nus.firmware, fw, sizeof(mp_nus.firmware) - 1);
                    mp_nus.firmware[sizeof(mp_nus.firmware) - 1] = '\0';
                    printf("[BTSTACK_HOST] DIS firmware revision: %s\n", mp_nus.firmware);
                }
            }
            break;
        }

        case GATTSERVICE_SUBEVENT_DEVICE_INFORMATION_DONE: {
            hci_con_handle_t handle = gattservice_subevent_device_information_done_get_con_handle(packet);
            uint8_t att_status = gattservice_subevent_device_information_done_get_att_status(packet);
            printf("[BTSTACK_HOST] DIS query done: handle=0x%04X status=0x%02X\n", handle, att_status);
            // Skip the standard Battery Service client for devices on the Valve GATT
            // path (Steam Controller 2): it has its own battery characteristic, and a
            // concurrent BAS query would contend with the Valve service discovery we
            // just kicked off in the PnP-ID handler above.
            ble_connection_t *dconn = find_connection_by_handle(handle);
            if (dconn && dconn->profile && dconn->profile->ble == BT_BLE_VALVE) {
                printf("[BTSTACK_HOST] DIS done: Valve path — skipping standard BAS client\n");
                break;
            }
            // Start Battery Service client after DIS completes (avoids GATT procedure contention)
            start_battery_service_client(handle);
            break;
        }

        default:
            break;
    }
}

static void hids_client_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size)
{
    UNUSED(packet_type);  // hids_client passes HCI_EVENT_GATTSERVICE_META, not HCI_EVENT_PACKET
    UNUSED(channel);
    UNUSED(size);

    // Check the event type in the packet itself
    if (hci_event_packet_get_type(packet) != HCI_EVENT_GATTSERVICE_META) return;

    switch (hci_event_gattservice_meta_get_subevent_code(packet)) {
        case GATTSERVICE_SUBEVENT_HID_SERVICE_CONNECTED: {
            uint8_t status = gattservice_subevent_hid_service_connected_get_status(packet);
            uint8_t num_instances = gattservice_subevent_hid_service_connected_get_num_instances(packet);
            printf("[BTSTACK_HOST] HIDS connected! status=%d instances=%d\n", status, num_instances);

            if (status == ERROR_CODE_SUCCESS) {
                // Route by the event's own cid so two BLE HID devices stay separate
                // (a shared global handle/cid cross-wired their descriptors/reports).
                uint16_t cid = gattservice_subevent_hid_service_connected_get_hids_cid(packet);
                ble_connection_t *conn = find_connection_by_hids_cid(cid);
                if (!conn) conn = find_connection_by_handle(hid_state.gatt_handle);  // fallback
                if (conn) {
                    conn->state = BLE_STATE_READY;
                    conn->hid_ready = true;
                    conn->hids_cid = cid;

                    // Assign conn_index if not already set
                    int slot = -1;
                    for (int i = 0; i < MAX_BLE_CONNECTIONS; i++) {
                        if (&hid_state.connections[i] == conn) {
                            conn->conn_index = BLE_CONN_INDEX_OFFSET + i;
                            slot = i;
                            break;
                        }
                    }

                    // Notify bthid layer that device is ready
                    btstack_host_stop_scan();
                    scan_timeout_end = 0;
                    printf("[BTSTACK_HOST] Calling bt_on_hid_ready(%d) for BLE device '%s'\n",
                           conn->conn_index, conn->name);
                    bt_on_hid_ready(conn->conn_index);

                    // Pass THIS device's HID descriptor to bthid (per-connection cid)
                    const uint8_t* hid_desc = hids_client_descriptor_storage_get_descriptor_data(conn->hids_cid, 0);
                    uint16_t hid_desc_len = hids_client_descriptor_storage_get_descriptor_len(conn->hids_cid, 0);
                    if (hid_desc && hid_desc_len > 0) {
                        printf("[BTSTACK_HOST] BLE HID descriptor: %d bytes\n", hid_desc_len);
                        bthid_set_hid_descriptor(conn->conn_index, hid_desc, hid_desc_len);
                    }

                    // NOTE: DIS (PnP VID/PID) and BAS (battery) are intentionally
                    // NOT started here. Each is a gatt_client query, and running
                    // them concurrently with hids_client_enable_notifications()
                    // starves the HID notification enabling on devices with many
                    // report characteristics (e.g. Augmental MouthPad, 4+ reports):
                    // the CCC writes never complete, no reports flow, and the
                    // device drops the link. They are deferred to the
                    // REPORTS_NOTIFICATION (0x1C) handler below, so only one GATT
                    // client uses the connection at a time.

                    // Switch to REPORT protocol mode BEFORE enabling notifications
                    // (mirrors the working mouthpad-usb order). The MouthPad boots
                    // in BOOT mode; switching mode AFTER subscribing makes it drop
                    // the report CCCs, so it never streams. The sequencer writes
                    // the mode (once hids_client is back in CONNECTED), THEN
                    // enables notifications, then the 0x1C handler starts DIS/BAS/NUS.
                    if (slot >= 0) {
                        mp_hid_setup[slot].active   = true;
                        mp_hid_setup[slot].handle   = conn->handle;
                        mp_hid_setup[slot].hids_cid = conn->hids_cid;
                        mp_hid_setup[slot].phase    = 0;
                        mp_hid_setup[slot].start_ms = btstack_run_loop_get_time_ms();
                    }
                }
            }
            break;
        }

        case GATTSERVICE_SUBEVENT_HID_SERVICE_REPORTS_NOTIFICATION: {
            uint8_t configuration = gattservice_subevent_hid_service_reports_notification_get_configuration(packet);
            printf("[BTSTACK_HOST] HID Reports Notification configured: %d\n", configuration);
            printf("[BTSTACK_HOST] Ready to receive HID reports!\n");

            // Mode is already REPORT and notifications are now enabled. Start the
            // remaining GATT clients one at a time: DIS -> BAS, then arm NUS.
            // Resolve THIS device by the event's cid (not the global handle).
            {
                uint16_t cid = gattservice_subevent_hid_service_reports_notification_get_hids_cid(packet);
                ble_connection_t* nconn = find_connection_by_hids_cid(cid);
                hci_con_handle_t nhandle = nconn ? nconn->handle : hid_state.gatt_handle;

                uint8_t dis = device_information_service_client_query(
                    nhandle, dis_client_handler);
                if (dis != ERROR_CODE_SUCCESS) {
                    start_battery_service_client(nhandle);
                }
                // Recognize the MouthPad by the live conn name OR the stored
                // last-connected name (a reconnect can leave conn->name empty,
                // which previously left the NUS relay stuck unarmed -> the app
                // shows "scanning" despite a paired MouthPad).
                if (nconn && (strstr(nconn->name, "MouthPad") != NULL ||
                              strstr(hid_state.last_connected_name, "MouthPad") != NULL ||
                              strstr(nconn->name, "JoypadOS") != NULL ||
                              strstr(hid_state.last_connected_name, "JoypadOS") != NULL)) {
                    mp_nus_mark_pending(nhandle);
                }
            }
            break;
        }

        case GATTSERVICE_SUBEVENT_HID_PROTOCOL_MODE: {
            uint8_t pm = gattservice_subevent_hid_protocol_mode_get_protocol_mode(packet);
            printf("[MP] device protocol mode now = %s (%d)\n",
                   pm == 0 ? "BOOT" : "REPORT", pm);
            break;
        }

        case GATTSERVICE_SUBEVENT_HID_REPORT: {
            uint16_t report_len = gattservice_subevent_hid_report_get_report_len(packet);
            const uint8_t *report = gattservice_subevent_hid_report_get_report(packet);

            // Route by the report's OWN cid -> the owning connection. Using the
            // global gatt_handle here sent BOTH devices' reports to the last one
            // connected (the haywire merge). This is the core 2-BLE-HID fix.
            uint16_t cid = gattservice_subevent_hid_report_get_hids_cid(packet);
            ble_connection_t* rconn = find_connection_by_hids_cid(cid);
            int conn_index = rconn ? rconn->conn_index : get_ble_conn_index_by_handle(hid_state.gatt_handle);
            if (conn_index >= 0) {
                route_ble_hid_report(conn_index, report, report_len);
            }

            // Forward to callback if set
            if (hid_state.report_callback) {
                hid_state.report_callback(hid_state.gatt_handle, report, report_len);
            }
            break;
        }

        default:
            printf("[BTSTACK_HOST] GATT service subevent: 0x%02X\n",
                   hci_event_gattservice_meta_get_subevent_code(packet));
            break;
    }
}

// ============================================================================
// HELPERS
// ============================================================================

static ble_connection_t* find_connection_by_handle(hci_con_handle_t handle)
{
    for (int i = 0; i < MAX_BLE_CONNECTIONS; i++) {
        if (hid_state.connections[i].handle == handle) {
            return &hid_state.connections[i];
        }
    }
    return NULL;
}

// Find the connection that owns a given BLE HID client id. Used to route
// hids_client events (connect/notification/report) to the right device when
// more than one BLE HID device is connected.
static ble_connection_t* find_connection_by_hids_cid(uint16_t hids_cid)
{
    if (hids_cid == 0) return NULL;
    for (int i = 0; i < MAX_BLE_CONNECTIONS; i++) {
        if (hid_state.connections[i].hids_cid == hids_cid) {
            return &hid_state.connections[i];
        }
    }
    return NULL;
}

static ble_connection_t* find_free_connection(void)
{
    for (int i = 0; i < MAX_BLE_CONNECTIONS; i++) {
        if (hid_state.connections[i].handle == HCI_CON_HANDLE_INVALID) {
            return &hid_state.connections[i];
        }
    }
    return NULL;
}

// ============================================================================
// STATUS
// ============================================================================

bool btstack_host_is_initialized(void)
{
    return hid_state.initialized;
}

bool btstack_host_is_powered_on(void)
{
    return hid_state.powered_on;
}

bool btstack_host_is_scanning(void)
{
    return hid_state.scan_active || classic_state.inquiry_active;
}

// ============================================================================
// CLASSIC BT HID HOST PACKET HANDLER
// ============================================================================

static bool btstack_report_debug_done = false;

static void hid_host_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size)
{
    UNUSED(channel);
    UNUSED(size);

    if (packet_type != HCI_EVENT_PACKET) return;

    uint8_t event_type = hci_event_packet_get_type(packet);
    if (event_type != HCI_EVENT_HID_META) return;

    uint8_t subevent = hci_event_hid_meta_get_subevent_code(packet);

    switch (subevent) {
        case HID_SUBEVENT_INCOMING_CONNECTION: {
            // Accept incoming HID connections from devices
            uint16_t hid_cid = hid_subevent_incoming_connection_get_hid_cid(packet);
            bd_addr_t incoming_addr;
            hid_subevent_incoming_connection_get_address(packet, incoming_addr);

            // For Wiimotes/Wii U Pro: accept HID Host connection for reconnection
            if (wiimote_conn.active && memcmp(incoming_addr, wiimote_conn.addr, 6) == 0) {
                printf("[BTSTACK_HOST] Wiimote HID incoming - accepting\n");
                wiimote_conn.using_hid_host = true;
                wiimote_conn.hid_host_cid = hid_cid;
                hid_host_accept_connection(hid_cid, HID_PROTOCOL_MODE_REPORT);

                // Allocate classic_connection slot for HID_SUBEVENT_CONNECTION_OPENED to find
                classic_connection_t* conn = find_free_classic_connection();
                if (conn) {
                    memset(conn, 0, sizeof(*conn));
                    conn->active = true;
                    conn->hid_cid = hid_cid;
                    memcpy(conn->addr, wiimote_conn.addr, 6);
                    memcpy(conn->class_of_device, wiimote_conn.class_of_device, 3);
                    strncpy(conn->name, wiimote_conn.name, sizeof(conn->name) - 1);
                    conn->vendor_id = 0x057E;  // Nintendo
                    conn->connect_time = btstack_run_loop_get_time_ms();
                    // Get index for wiimote_conn
                    for (int i = 0; i < MAX_CLASSIC_CONNECTIONS; i++) {
                        if (&classic_state.connections[i] == conn) {
                            wiimote_conn.conn_index = i;
                            printf("[BTSTACK_HOST] Wiimote: allocated conn_index=%d for HID Host\n", i);
                            break;
                        }
                    }
                }
                break;
            }

            // Determine protocol mode from device profile (if name is available)
            hid_protocol_mode_t accept_mode = HID_PROTOCOL_MODE_REPORT_WITH_FALLBACK_TO_BOOT;
            if (classic_state.pending_valid && classic_state.pending_name[0]) {
                const bt_device_profile_t* profile = bt_device_lookup_by_name(classic_state.pending_name);
                if (profile->hid_mode == BT_HID_MODE_REPORT) {
                    accept_mode = HID_PROTOCOL_MODE_REPORT;
                }
            }
            printf("[BTSTACK_HOST] HID incoming connection, cid=0x%04X - accepting (mode=%s)\n",
                   hid_cid, accept_mode == HID_PROTOCOL_MODE_REPORT ? "REPORT" : "FALLBACK");
            hid_host_accept_connection(hid_cid, accept_mode);

            // Allocate connection slot if needed
            if (!find_classic_connection_by_cid(hid_cid)) {
                classic_connection_t* conn = find_free_classic_connection();
                if (conn) {
                    memset(conn, 0, sizeof(*conn));
                    conn->active = true;
                    conn->hid_cid = hid_cid;
                    hid_subevent_incoming_connection_get_address(packet, conn->addr);

                    // Use pending COD and name if address matches (from HCI_EVENT_CONNECTION_REQUEST)
                    if (classic_state.pending_valid &&
                        memcmp(conn->addr, classic_state.pending_addr, 6) == 0) {
                        conn->class_of_device[0] = classic_state.pending_cod & 0xFF;
                        conn->class_of_device[1] = (classic_state.pending_cod >> 8) & 0xFF;
                        conn->class_of_device[2] = (classic_state.pending_cod >> 16) & 0xFF;
                        // Copy name if we got it from remote name request
                        if (classic_state.pending_name[0]) {
                            strncpy(conn->name, classic_state.pending_name, sizeof(conn->name) - 1);
                            conn->name[sizeof(conn->name) - 1] = '\0';
                            printf("[BTSTACK_HOST] Using pending name: %s\n", conn->name);
                        }
                        // Copy VID/PID if we got them from SDP query
                        if (classic_state.pending_vid || classic_state.pending_pid) {
                            conn->vendor_id = classic_state.pending_vid;
                            conn->product_id = classic_state.pending_pid;
                            printf("[BTSTACK_HOST] Using pending VID/PID: 0x%04X/0x%04X\n",
                                   conn->vendor_id, conn->product_id);
                        }
                        // DON'T clear pending_valid here - PIN code request may come after this
                        // It will be cleared in HID_SUBEVENT_CONNECTION_OPENED
                        printf("[BTSTACK_HOST] Using pending COD: 0x%06X\n", (unsigned)classic_state.pending_cod);
                    }
                    conn->connect_time = btstack_run_loop_get_time_ms();
                }
            }
            break;
        }

        case HID_SUBEVENT_CONNECTION_OPENED: {
            uint16_t hid_cid = hid_subevent_connection_opened_get_hid_cid(packet);
            uint8_t status = hid_subevent_connection_opened_get_status(packet);

            // Reset security level if we elevated it for Wiimote
            if (classic_state.pending_hid_connect) {
                printf("[BTSTACK_HOST] Resetting security level to 0\n");
                gap_set_security_level(LEVEL_0);
                classic_state.pending_hid_connect = false;
            }

            // Clear pending connection info now that HID is established
            classic_state.pending_valid = false;

            if (status != ERROR_CODE_SUCCESS) {
                printf("[BTSTACK_HOST] HID connection failed, cid=0x%04X status=0x%02X\n", hid_cid, status);
                // Remove connection slot
                classic_connection_t* conn = find_classic_connection_by_cid(hid_cid);
                if (conn) {
                    memset(conn, 0, sizeof(*conn));
                }

                // If this was an outgoing connection, disconnect ACL and wait for
                // the device to reconnect via the incoming path. Some controllers
                // (certain DS4 HW revisions) don't accept HID L2CAP channels from
                // the host but work when they initiate the connection themselves.
                // A link key was exchanged during the failed attempt, so when the
                // device reconnects (incoming), authentication will use the stored key.
                // Don't resume scanning — otherwise we'll rediscover the device
                // still in pairing mode and loop endlessly.
                if (!hid_subevent_connection_opened_get_incoming(packet)) {
                    hci_con_handle_t con_handle = hid_subevent_connection_opened_get_con_handle(packet);
                    printf("[BTSTACK_HOST] Outgoing HID failed, disconnecting to allow incoming reconnect\n");
                    gap_disconnect(con_handle);
                    classic_state.pending_outgoing = false;
                    classic_state.pending_valid = false;
                    // Don't scan — stay connectable, wait for incoming reconnection
                    classic_state.waiting_for_incoming_time = btstack_run_loop_get_time_ms();
                }
                return;
            }

            printf("[BTSTACK_HOST] HID connection opened, cid=0x%04X\n", hid_cid);

            // Mark connection as ready (HID channels established)
            classic_connection_t* conn = find_classic_connection_by_cid(hid_cid);
            if (conn) {
                conn->hid_ready = true;

                // PnP (Device ID) SDP query for the hid_host path.
                //
                // The only other call site is gated on
                //     classic_state.pending_hid_connect && wiimote_conn.active
                // i.e. the direct-L2CAP path, so a pad that comes up through
                // HID Host is never queried and product_id stays 0. The
                // remote-name handler then fills in vendor_id from the name
                // profile default while bt_device_wiimote_pid_from_name()
                // returns 0 for non-Wiimotes, leaving 054C:0000 for a
                // DualSense -- which ds5_match() rejects, so the looser
                // ds4_match() claims it and the pad produces no input.
                if (conn->product_id == 0) {
                    memcpy(classic_state.pending_addr, conn->addr, 6);
                    classic_state.pending_vid = 0;
                    classic_state.pending_pid = 0;
                    uint8_t sdp_rc = sdp_client_query_uuid16(
                        &sdp_query_vid_pid_callback, conn->addr,
                        BLUETOOTH_SERVICE_CLASS_PNP_INFORMATION);
                    printf("[BTSTACK_HOST] PnP SDP query for '%s': status=0x%02X\n",
                           conn->name, sdp_rc);
                }

                // Check if this is a direct-L2CAP device by profile or name
                bool is_direct_l2cap = (conn->profile &&
                                        conn->profile->classic == BT_CLASSIC_DIRECT_L2CAP);
                // Also check by name if profile wasn't set (late name resolution)
                if (!is_direct_l2cap && conn->name[0]) {
                    const bt_device_profile_t* conn_profile = bt_device_lookup_by_name(conn->name);
                    if (conn_profile->classic == BT_CLASSIC_DIRECT_L2CAP) {
                        is_direct_l2cap = true;
                    }
                    if (!conn->profile) {
                        conn->profile = conn_profile;
                    }
                }
                // Also check wiimote_conn state (may have been set up during inquiry)
                if (!is_direct_l2cap && wiimote_conn.active &&
                    memcmp(conn->addr, wiimote_conn.addr, 6) == 0) {
                    is_direct_l2cap = true;
                }

                if (is_direct_l2cap) {
                    // Direct L2CAP devices: HID Host handles receiving, we send via direct L2CAP
                    printf("[BTSTACK_HOST] %s HID connected via HID Host (receive via HID Host, send via L2CAP)\n",
                           conn->profile ? conn->profile->name : "Wiimote");

                    // Set default VID/PID from profile if not already set
                    if (conn->vendor_id == 0 && conn->profile && conn->profile->default_vid) {
                        conn->vendor_id = conn->profile->default_vid;
                    }
                    if (conn->product_id == 0) {
                        uint16_t pid = bt_device_wiimote_pid_from_name(conn->name);
                        if (pid) {
                            conn->product_id = pid;
                            printf("[BTSTACK_HOST] Detected %s by name, PID=0x%04X\n",
                                   conn->profile ? conn->profile->name : "device", pid);
                        }
                    }

                    // Initialize wiimote_conn if not already active (e.g., incoming
                    // reconnection where name wasn't available at CONNECTION_COMPLETE)
                    if (!wiimote_conn.active) {
                        memset(&wiimote_conn, 0, sizeof(wiimote_conn));
                        wiimote_conn.active = true;
                        wiimote_conn.state = WIIMOTE_STATE_IDLE;
                        memcpy(wiimote_conn.addr, conn->addr, 6);
                        memcpy(wiimote_conn.class_of_device, conn->class_of_device, 3);
                        wiimote_conn.using_hid_host = true;
                        wiimote_conn.hid_host_cid = hid_cid;
                    }

                    // Link wiimote_conn to this classic_connection slot for routing
                    int conn_index = get_classic_conn_index(hid_cid);
                    if (conn_index >= 0) {
                        wiimote_conn.conn_index = conn_index;
                        wiimote_conn.vendor_id = conn->vendor_id;
                        wiimote_conn.product_id = conn->product_id;
                        strncpy(wiimote_conn.name, conn->name, sizeof(wiimote_conn.name) - 1);

                        bthid_update_device_info(conn_index, conn->name,
                                                 conn->vendor_id, conn->product_id);

                        // NOTE: We previously called hid_host_get_l2cap_cids() here to get L2CAP CIDs
                        // from HID Host for direct L2CAP sending (Wiimotes don't support SET_PROTOCOL).
                        // This required a custom BTstack patch (see .dev/docs/btstack-patches.md).
                        // Removed because CI uses upstream BTstack without the patch.
                        // If Wiimote HID Host mode has issues, consider re-adding the patch.

                        printf("[BTSTACK_HOST] Wiimote: conn_index=%d control_cid=0x%04X interrupt_cid=0x%04X using_hid_host=%d\n",
                               conn_index, wiimote_conn.control_cid, wiimote_conn.interrupt_cid, wiimote_conn.using_hid_host);

                        if (wiimote_conn.using_hid_host) {
                            // Using HID Host — set up state but defer bt_on_hid_ready
                            // to DESCRIPTOR_AVAILABLE. BTstack's HID Host immediately
                            // starts SDP after CONNECTION_OPENED (state → W2_SEND_SDP_QUERY),
                            // and hid_host_send_report() fails with COMMAND_DISALLOWED
                            // until SDP + SET_PROTOCOL complete. Deferring ensures the
                            // driver's init subcommands (SET_INPUT_MODE etc.) succeed.
                            wiimote_conn.hid_host_ready = true;
                            wiimote_conn.state = WIIMOTE_STATE_CONNECTED;
                            btstack_host_stop_scan();
                            scan_timeout_end = 0;
                            printf("[BTSTACK_HOST] Wiimote: HID Host ready, deferring bt_on_hid_ready to DESCRIPTOR_AVAILABLE\n");
                        } else if (wiimote_conn.control_cid != 0 && wiimote_conn.interrupt_cid != 0) {
                            printf("[BTSTACK_HOST] Wiimote: calling bt_on_hid_ready(%d) via direct L2CAP\n", conn_index);
                            bt_on_hid_ready(conn_index);
                        } else {
                            printf("[BTSTACK_HOST] Wiimote: waiting for L2CAP CIDs before ready\n");
                        }
                    }
                } else {
                    // Set default VID/PID from profile if available
                    if (conn->vendor_id == 0 && conn->profile && conn->profile->default_vid) {
                        conn->vendor_id = conn->profile->default_vid;
                        printf("[BTSTACK_HOST] Set VID=0x%04X from %s profile\n",
                               conn->vendor_id, conn->profile->name);
                    }

                    // Non-Wiimote: wait for HID_SUBEVENT_DESCRIPTOR_AVAILABLE
                    // NOTE: Do NOT issue SDP queries here — BTstack HID Host starts its
                    // own SDP query (for HID descriptor) immediately after CONNECTION_OPENED.
                    // sdp_client only handles one query at a time, so issuing ours here
                    // would block BTstack's, preventing DESCRIPTOR_AVAILABLE from firing.
                    // VID/PID SDP query is deferred to DESCRIPTOR_AVAILABLE instead.
                }
            }
            break;
        }

        case HID_SUBEVENT_DESCRIPTOR_AVAILABLE: {
            uint16_t hid_cid = hid_subevent_descriptor_available_get_hid_cid(packet);
            uint8_t status = hid_subevent_descriptor_available_get_status(packet);

            printf("[BTSTACK_HOST] HID descriptor available, cid=0x%04X status=0x%02X\n", hid_cid, status);

            // Notify bthid layer that device is ready
            // This fires after SDP + SET_PROTOCOL complete, so BTstack's state
            // is CONNECTION_ESTABLISHED and hid_host_send_report() will succeed.
            int conn_index = get_classic_conn_index(hid_cid);
            if (conn_index >= 0) {
                // Pass HID descriptor to bthid for generic gamepad parsing
                const uint8_t* hid_desc = hid_descriptor_storage_get_descriptor_data(hid_cid);
                uint16_t hid_desc_len = hid_descriptor_storage_get_descriptor_len(hid_cid);
                if (hid_desc && hid_desc_len > 0) {
                    printf("[BTSTACK_HOST] Classic HID descriptor: %d bytes\n", hid_desc_len);
                    bthid_set_hid_descriptor(conn_index, hid_desc, hid_desc_len);
                }

                btstack_host_stop_scan();
                scan_timeout_end = 0;
                printf("[BTSTACK_HOST] Calling bt_on_hid_ready(%d)\n", conn_index);
                bt_on_hid_ready(conn_index);

                // Query VID/PID via SDP if not yet known (deferred from CONNECTION_OPENED
                // to avoid conflicting with BTstack's internal HID descriptor SDP query)
                classic_connection_t* desc_conn = find_classic_connection_by_cid(hid_cid);
                if (desc_conn && desc_conn->vendor_id == 0 && desc_conn->product_id == 0) {
                    memcpy(classic_state.pending_addr, desc_conn->addr, 6);
                    classic_state.pending_vid = 0;
                    classic_state.pending_pid = 0;
                    printf("[BTSTACK_HOST] Querying VID/PID via SDP (deferred)\n");
                    sdp_client_query_uuid16(&sdp_query_vid_pid_callback, desc_conn->addr,
                                            BLUETOOTH_SERVICE_CLASS_PNP_INFORMATION);
                }
            }
            break;
        }

        case HID_SUBEVENT_REPORT: {
            uint16_t hid_cid = hid_subevent_report_get_hid_cid(packet);
            const uint8_t* report = hid_subevent_report_get_report(packet);
            uint16_t report_len = hid_subevent_report_get_report_len(packet);

            // Debug: show raw BTstack report
            if (!btstack_report_debug_done && report_len >= 4) {
                printf("[BTSTACK_HOST] Raw report len=%d: %02X %02X %02X %02X\n",
                       report_len, report[0], report[1], report[2], report[3]);
                btstack_report_debug_done = true;
            }

            // Route to bthid layer
            // BTstack report already includes 0xA1 header (DATA|INPUT)
            int conn_index = get_classic_conn_index(hid_cid);
            if (conn_index >= 0 && report_len > 0) {
                bt_on_hid_report(conn_index, report, report_len);
            }
            break;
        }

        case HID_SUBEVENT_CONNECTION_CLOSED: {
            uint16_t hid_cid = hid_subevent_connection_closed_get_hid_cid(packet);
            printf("[BTSTACK_HOST] HID connection closed, cid=0x%04X\n", hid_cid);

            // Reset debug flag so reconnections produce debug output
            btstack_report_debug_done = false;

            // Notify bthid layer
            int conn_index = get_classic_conn_index(hid_cid);
            if (conn_index >= 0) {
                bt_on_disconnect(conn_index);
            }

            // Free connection slot
            classic_connection_t* conn = find_classic_connection_by_cid(hid_cid);
            if (conn) {
                memset(conn, 0, sizeof(*conn));
            }

            // Resume scanning if no devices remain
            if (btstack_classic_get_connection_count() == 0) {
                printf("[BTSTACK_HOST] No devices connected, resuming scan\n");
                btstack_host_start_scan();
            }
            break;
        }

        case HID_SUBEVENT_SET_PROTOCOL_RESPONSE: {
            uint16_t hid_cid = hid_subevent_set_protocol_response_get_hid_cid(packet);
            uint8_t handshake = hid_subevent_set_protocol_response_get_handshake_status(packet);
            hid_protocol_mode_t mode = hid_subevent_set_protocol_response_get_protocol_mode(packet);
            printf("[BTSTACK_HOST] HID set protocol response: cid=0x%04X handshake=%d mode=%d\n",
                   hid_cid, handshake, mode);
            break;
        }

        default:
            printf("[BTSTACK_HOST] HID subevent: 0x%02X\n", subevent);
            break;
    }
}

// ============================================================================
// WIIMOTE DIRECT L2CAP PACKET HANDLER
// ============================================================================

static void wiimote_l2cap_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size)
{
    UNUSED(channel);

    switch (packet_type) {
        case HCI_EVENT_PACKET: {
            uint8_t event_type = hci_event_packet_get_type(packet);

            if (event_type == L2CAP_EVENT_CHANNEL_OPENED) {
                uint8_t status = l2cap_event_channel_opened_get_status(packet);
                uint16_t local_cid = l2cap_event_channel_opened_get_local_cid(packet);
                uint16_t psm = l2cap_event_channel_opened_get_psm(packet);

                printf("[BTSTACK_HOST] Wiimote L2CAP opened: status=%d PSM=0x%04X cid=0x%04X\n",
                       status, psm, local_cid);

                if (status != 0) {
                    printf("[BTSTACK_HOST] Wiimote: L2CAP channel failed: 0x%02X\n", status);
                    // Don't deactivate - wait for HID Host to handle via HID_SUBEVENT_INCOMING_CONNECTION
                    // (timing varies: HID incoming may come before or after L2CAP failure)
                    printf("[BTSTACK_HOST] Wiimote: waiting for HID Host fallback\n");
                    return;
                }

                if (psm == PSM_HID_CONTROL && wiimote_conn.state == WIIMOTE_STATE_W4_CONTROL_CONNECTED) {
                    // Control channel opened, now create interrupt channel
                    printf("[BTSTACK_HOST] Wiimote: Control channel connected, creating Interrupt channel (PSM 0x13)...\n");

                    uint16_t interrupt_cid;
                    uint8_t l2cap_status = l2cap_create_channel(wiimote_l2cap_packet_handler,
                                                                wiimote_conn.addr,
                                                                PSM_HID_INTERRUPT,
                                                                0xFFFF,
                                                                &interrupt_cid);
                    if (l2cap_status == ERROR_CODE_SUCCESS) {
                        wiimote_conn.interrupt_cid = interrupt_cid;
                        wiimote_conn.state = WIIMOTE_STATE_W4_INTERRUPT_CONNECTED;
                        printf("[BTSTACK_HOST] Wiimote: L2CAP interrupt channel request sent, cid=0x%04X\n", interrupt_cid);
                    } else {
                        printf("[BTSTACK_HOST] Wiimote: l2cap_create_channel (interrupt) failed: 0x%02X\n", l2cap_status);
                        wiimote_conn.active = false;
                        classic_state.pending_hid_connect = false;
                    }

                } else if (psm == PSM_HID_INTERRUPT && wiimote_conn.state == WIIMOTE_STATE_W4_INTERRUPT_CONNECTED) {
                    // Interrupt channel opened - connection complete!
                    printf("[BTSTACK_HOST] Wiimote: Interrupt channel connected - HID READY!\n");
                    wiimote_conn.state = WIIMOTE_STATE_CONNECTED;
                    classic_state.pending_hid_connect = false;

                    // Stop scanning now that we have a connected device
                    btstack_host_stop_scan();
                    scan_timeout_end = 0;

                    // Allocate classic connection slot if not already allocated (reconnection case)
                    if (wiimote_conn.conn_index < 0) {
                        classic_connection_t* conn = find_free_classic_connection();
                        if (conn) {
                            memset(conn, 0, sizeof(*conn));
                            conn->active = true;
                            conn->hid_cid = 0xFFFF;  // Mark as Wiimote (no HID Host CID)
                            memcpy(conn->addr, wiimote_conn.addr, 6);
                            strncpy(conn->name, wiimote_conn.name, sizeof(conn->name) - 1);
                            conn->vendor_id = 0x057E;  // Nintendo
                            conn->product_id = bt_device_wiimote_pid_from_name(wiimote_conn.name);
                            conn->hid_ready = true;

                            // Get index
                            for (int i = 0; i < MAX_CLASSIC_CONNECTIONS; i++) {
                                if (&classic_state.connections[i] == conn) {
                                    wiimote_conn.conn_index = i;
                                    wiimote_conn.vendor_id = conn->vendor_id;
                                    wiimote_conn.product_id = conn->product_id;
                                    printf("[BTSTACK_HOST] Wiimote: allocated conn_index=%d\n", i);
                                    break;
                                }
                            }
                        }
                    }

                    // Update the classic connection slot
                    if (wiimote_conn.conn_index >= 0 && wiimote_conn.conn_index < MAX_CLASSIC_CONNECTIONS) {
                        classic_connection_t* conn = &classic_state.connections[wiimote_conn.conn_index];
                        conn->hid_ready = true;

                        // Update bthid with device info
                        // Use SDP VID/PID if available, otherwise default to Nintendo (0x057E)
                        uint16_t vid = wiimote_conn.vendor_id ? wiimote_conn.vendor_id : 0x057E;
                        uint16_t pid = wiimote_conn.product_id;
                        printf("[BTSTACK_HOST] Wiimote: updating bthid with name='%s' VID=0x%04X PID=0x%04X\n",
                               wiimote_conn.name, vid, pid);
                        bthid_update_device_info(wiimote_conn.conn_index, wiimote_conn.name, vid, pid);

                        // Notify bthid layer
                        printf("[BTSTACK_HOST] Wiimote: calling bt_on_hid_ready(%d)\n", wiimote_conn.conn_index);
                        bt_on_hid_ready(wiimote_conn.conn_index);
                    }
                }

            } else if (event_type == L2CAP_EVENT_CHANNEL_CLOSED) {
                uint16_t local_cid = l2cap_event_channel_closed_get_local_cid(packet);
                printf("[BTSTACK_HOST] Wiimote L2CAP closed: cid=0x%04X\n", local_cid);

                if (wiimote_conn.active &&
                    (local_cid == wiimote_conn.control_cid || local_cid == wiimote_conn.interrupt_cid)) {
                    // Notify disconnect
                    if (wiimote_conn.conn_index >= 0) {
                        bt_on_disconnect(wiimote_conn.conn_index);
                        // Clear connection slot
                        if (wiimote_conn.conn_index < MAX_CLASSIC_CONNECTIONS) {
                            memset(&classic_state.connections[wiimote_conn.conn_index], 0, sizeof(classic_connection_t));
                        }
                    }
                    memset(&wiimote_conn, 0, sizeof(wiimote_conn));
                }
            }
            break;
        }

        case L2CAP_DATA_PACKET: {
            // HID data from Wiimote interrupt channel
            // Data already includes HID header (0xA1 for DATA|INPUT)
            if (wiimote_conn.active && wiimote_conn.state == WIIMOTE_STATE_CONNECTED) {
                // Route to bthid layer
                if (wiimote_conn.conn_index >= 0 && size > 0) {
                    bt_on_hid_report(wiimote_conn.conn_index, packet, size);
                }
            } else {
                printf("[BTSTACK_HOST] Wiimote data dropped: active=%d state=%d\n",
                       wiimote_conn.active, wiimote_conn.state);
            }
            break;
        }

        default:
            break;
    }
}

// ============================================================================
// CLASSIC BT OUTPUT REPORTS
// ============================================================================

// Send SET_REPORT on control channel with specified report type
// report_type: 1=Input, 2=Output, 3=Feature
bool btstack_classic_send_set_report_type(uint8_t conn_index, uint8_t report_type,
                                           uint8_t report_id, const uint8_t* data, uint16_t len)
{
    if (conn_index >= MAX_CLASSIC_CONNECTIONS) return false;

    classic_connection_t* conn = &classic_state.connections[conn_index];
    if (!conn->active || !conn->hid_ready) return false;

    // Check if this is a Wiimote/direct L2CAP connection (marked with hid_cid = 0xFFFF)
    if (conn->hid_cid == 0xFFFF && wiimote_conn.active &&
        wiimote_conn.conn_index == conn_index &&
        wiimote_conn.state == WIIMOTE_STATE_CONNECTED) {
        // Send SET_REPORT on control channel via raw L2CAP
        // HID transaction format: [SET_REPORT | report_type] [report_id] [data...]
        static uint8_t wiimote_setreport_buf[80];
        uint16_t total = len + 2;
        if (total > sizeof(wiimote_setreport_buf)) return false;
        wiimote_setreport_buf[0] = 0x50 | (report_type & 0x03);  // SET_REPORT | type
        wiimote_setreport_buf[1] = report_id;
        if (len > 0) memcpy(wiimote_setreport_buf + 2, data, len);
        uint8_t status = l2cap_send(wiimote_conn.control_cid, wiimote_setreport_buf, total);
        if (status != ERROR_CODE_SUCCESS) {
            printf("[BTSTACK_HOST] wiimote send_set_report failed: type=%d id=0x%02X status=%d\n",
                   report_type, report_id, status);
        }
        return status == ERROR_CODE_SUCCESS;
    }

    // Map report type to BTstack enum
    hid_report_type_t hid_type;
    switch (report_type) {
        case 1: hid_type = HID_REPORT_TYPE_INPUT; break;
        case 2: hid_type = HID_REPORT_TYPE_OUTPUT; break;
        case 3: hid_type = HID_REPORT_TYPE_FEATURE; break;
        default: hid_type = HID_REPORT_TYPE_OUTPUT; break;
    }

    // hid_host_send_set_report stores a pointer to the data and sends asynchronously.
    // Copy into static buffer so the data persists until the actual L2CAP send completes.
    static uint8_t hid_host_set_report_buf[80];
    if (len > sizeof(hid_host_set_report_buf)) return false;
    if (len > 0) memcpy(hid_host_set_report_buf, data, len);

    uint8_t status = hid_host_send_set_report(conn->hid_cid, hid_type, report_id, hid_host_set_report_buf, len);
    if (status != ERROR_CODE_SUCCESS) {
        printf("[BTSTACK_HOST] send_set_report failed: type=%d id=0x%02X status=%d\n",
               report_type, report_id, status);
    }
    return status == ERROR_CODE_SUCCESS;
}

// Send SET_REPORT on control channel (default to OUTPUT type)
bool btstack_classic_send_set_report(uint8_t conn_index, uint8_t report_id,
                                      const uint8_t* data, uint16_t len)
{
    return btstack_classic_send_set_report_type(conn_index, 2, report_id, data, len);
}

// Send DATA on interrupt channel (for regular output reports)
bool btstack_classic_send_report(uint8_t conn_index, uint8_t report_id,
                                  const uint8_t* data, uint16_t len)
{
    // BLE connection — use GATT HIDS client
    if (conn_index >= BLE_CONN_INDEX_OFFSET) {
        uint8_t ble_index = conn_index - BLE_CONN_INDEX_OFFSET;
        if (ble_index >= MAX_BLE_CONNECTIONS) return false;
        ble_connection_t* conn = &hid_state.connections[ble_index];
        if (conn->handle == HCI_CON_HANDLE_INVALID || !conn->hid_ready) return false;
        if (conn->hids_cid == 0) return false;
        uint8_t status = hids_client_send_write_report(conn->hids_cid, report_id,
                                                        HID_REPORT_TYPE_OUTPUT,
                                                        data, len);
        if (status != ERROR_CODE_SUCCESS) {
            printf("[BTSTACK_HOST] BLE send_write_report failed: report_id=0x%02X status=0x%02X\n",
                   report_id, status);
        }
        return status == ERROR_CODE_SUCCESS;
    }

    if (conn_index >= MAX_CLASSIC_CONNECTIONS) return false;

    classic_connection_t* conn = &classic_state.connections[conn_index];
    if (!conn->active || !conn->hid_ready) return false;

    // Check if this is a Wiimote (direct L2CAP, marked with hid_cid = 0xFFFF)
    if (conn->hid_cid == 0xFFFF && wiimote_conn.active &&
        wiimote_conn.conn_index == conn_index &&
        wiimote_conn.state == WIIMOTE_STATE_CONNECTED) {
        // Build HID packet: 0xA2 (DATA|OUTPUT) + report_id + data
        // Buffer must fit DS5 BT output (79 bytes: 0xA2 + 78-byte report with CRC)
        static uint8_t wiimote_send_buf[80];
        if (len + 2 > sizeof(wiimote_send_buf)) return false;
        wiimote_send_buf[0] = 0xA2;  // DATA | OUTPUT
        wiimote_send_buf[1] = report_id;
        memcpy(wiimote_send_buf + 2, data, len);
        return l2cap_send(wiimote_conn.interrupt_cid, wiimote_send_buf, len + 2) == ERROR_CODE_SUCCESS;
    }

    // hid_host_send_report stores a pointer to the data and sends asynchronously.
    // Copy into static buffer so the data persists until the actual L2CAP send completes.
    // (DS5 audio report 0x36 does NOT go through here — it uses
    // btstack_classic_send_interrupt_raw with a captured L2CAP CID.)
    static uint8_t hid_host_report_buf[80];
    if (len > sizeof(hid_host_report_buf)) return false;
    if (len > 0) memcpy(hid_host_report_buf, data, len);

    return hid_host_send_report(conn->hid_cid, report_id, hid_host_report_buf, len) == ERROR_CODE_SUCCESS;
}

#ifdef CONFIG_DS5_DROP_SCREAM
// Send a prebuilt HID interrupt packet (0xA2 + report incl. CRC) directly on
// the L2CAP interrupt channel, bypassing the HID Host send state machine.
// Audio streaming (DS5 report 0x36 at ~94Hz) needs per-packet can-send-now
// pacing that hid_host_send_report's single-pending-report design can't give:
// fire-and-forget through it drops frames whenever the event loop lags a slot.
// The CID comes from our L2CAP_EVENT_CHANNEL_OPENED capture — no BTstack mods.
bool btstack_classic_send_interrupt_raw(uint8_t conn_index, const uint8_t* data, uint16_t len)
{
    if (conn_index >= MAX_CLASSIC_CONNECTIONS) return false;
    classic_connection_t* conn = &classic_state.connections[conn_index];
    if (!conn->active || !conn->hid_ready) return false;

    uint16_t interrupt_cid = 0;
    if (conn->hid_cid == 0xFFFF) {
        // Direct-L2CAP path (Sony-on-CYW43 / Wiimote outgoing connections):
        // the channels are our own — CIDs live in wiimote_conn.
        if (wiimote_conn.active && wiimote_conn.conn_index == conn_index) {
            interrupt_cid = wiimote_conn.interrupt_cid;
        }
    } else {
        for (int ci = 0; ci < MAX_CLASSIC_CONNECTIONS; ci++) {
            if (hid_intr_cids[ci].cid != 0 &&
                memcmp(hid_intr_cids[ci].addr, conn->addr, 6) == 0) {
                interrupt_cid = hid_intr_cids[ci].cid;
                break;
            }
        }
    }
    if (interrupt_cid == 0) return false;
    if (!l2cap_can_send_packet_now(interrupt_cid)) return false;

    // l2cap_send copies into the HCI outgoing buffer; caller's buffer need not persist
    return l2cap_send(interrupt_cid, (uint8_t*)data, len) == ERROR_CODE_SUCCESS;
}
#endif

// Check if a connection is a Wiimote (using direct L2CAP)
bool btstack_wiimote_is_connection(uint8_t conn_index)
{
    if (conn_index >= MAX_CLASSIC_CONNECTIONS) return false;
    classic_connection_t* conn = &classic_state.connections[conn_index];
    // Wiimote connections are marked with hid_cid = 0xFFFF
    return conn->active && conn->hid_cid == 0xFFFF &&
           wiimote_conn.active && wiimote_conn.conn_index == conn_index;
}

// Check if we can send on Wiimote L2CAP channel
bool btstack_wiimote_can_send(uint8_t conn_index)
{
    if (!wiimote_conn.active) {
        return false;
    }

    // Prefer direct L2CAP when we have the interrupt CID
    if (wiimote_conn.interrupt_cid != 0) {
        return l2cap_can_send_packet_now(wiimote_conn.interrupt_cid) != 0;
    }

    // Fallback to HID Host path
    if (wiimote_conn.using_hid_host && wiimote_conn.hid_host_ready) {
        return true;  // HID Host handles flow control internally
    }

    return false;
}

// Send raw L2CAP data to Wiimote on INTERRUPT channel
bool btstack_wiimote_send_raw(uint8_t conn_index, const uint8_t* data, uint16_t len)
{
    printf("[BTSTACK_HOST] wiimote_send_raw: active=%d using_hid=%d hid_ready=%d int_cid=0x%04X\n",
           wiimote_conn.active, wiimote_conn.using_hid_host, wiimote_conn.hid_host_ready, wiimote_conn.interrupt_cid);

    if (!wiimote_conn.active) {
        printf("[BTSTACK_HOST] wiimote_send_raw: no active connection\n");
        return false;
    }
    if (len == 0 || len > 80) {
        printf("[BTSTACK_HOST] wiimote_send_raw: bad len=%d\n", len);
        return false;
    }

    // Prefer direct L2CAP when we have the interrupt CID (works even with HID Host)
    // This bypasses hid_host_send_report which can fail with 0x0C if HID Host state isn't ready
    if (wiimote_conn.interrupt_cid != 0) {
        if (!l2cap_can_send_packet_now(wiimote_conn.interrupt_cid)) {
            printf("[BTSTACK_HOST] wiimote_send_raw: L2CAP not ready to send\n");
            return false;
        }

        uint8_t status = l2cap_send(wiimote_conn.interrupt_cid, data, len);
        if (status != ERROR_CODE_SUCCESS) {
            printf("[BTSTACK_HOST] wiimote_send_raw: l2cap_send failed status=0x%02X\n", status);
        } else {
            printf("[BTSTACK_HOST] wiimote_send_raw: sent %d bytes on INTR cid=0x%04X (0x%02X 0x%02X...)\n",
                   len, wiimote_conn.interrupt_cid, data[0], len > 1 ? data[1] : 0);
        }
        return status == ERROR_CODE_SUCCESS;
    }

    // Fallback to HID Host when using_hid_host but no direct CID (shouldn't happen normally)
    if (wiimote_conn.using_hid_host && wiimote_conn.hid_host_ready) {
        // Data format: first byte is 0xA2, second is report ID, rest is data
        if (len < 2) return false;
        uint8_t report_id = data[1];
        uint16_t payload_len = len - 2;
        // hid_host_send_report stores a pointer — copy to static buffer for async send
        static uint8_t wiimote_hid_report_buf[80];
        if (payload_len > sizeof(wiimote_hid_report_buf)) return false;
        if (payload_len > 0) memcpy(wiimote_hid_report_buf, &data[2], payload_len);
        printf("[BTSTACK_HOST] wiimote_send_raw via HID Host: cid=0x%04X report=0x%02X len=%d\n",
               wiimote_conn.hid_host_cid, report_id, payload_len);
        uint8_t status = hid_host_send_report(wiimote_conn.hid_host_cid, report_id, wiimote_hid_report_buf, payload_len);
        if (status == ERROR_CODE_SUCCESS) {
            printf("[BTSTACK_HOST] wiimote_send_raw: sent %d bytes via HID Host\n", len);
        } else {
            printf("[BTSTACK_HOST] wiimote_send_raw: HID Host send failed status=0x%02X\n", status);
        }
        return status == ERROR_CODE_SUCCESS;
    }

    printf("[BTSTACK_HOST] wiimote_send_raw: no interrupt CID and HID Host not ready\n");
    return false;
}

// Send raw L2CAP data to Wiimote on CONTROL channel
bool btstack_wiimote_send_control(uint8_t conn_index, const uint8_t* data, uint16_t len)
{
    printf("[BTSTACK_HOST] wiimote_send_control: idx=%d len=%d control_cid=0x%04X using_hid_host=%d\n",
           conn_index, len, wiimote_conn.control_cid, wiimote_conn.using_hid_host);

    if (!wiimote_conn.active) {
        printf("[BTSTACK_HOST] wiimote_send_control: no active connection\n");
        return false;
    }
    if (len == 0 || len > 64) {
        printf("[BTSTACK_HOST] wiimote_send_control: bad len=%d\n", len);
        return false;
    }

    // Prefer direct L2CAP when we have the control CID (works even with HID Host)
    if (wiimote_conn.control_cid != 0) {
        if (!l2cap_can_send_packet_now(wiimote_conn.control_cid)) {
            printf("[BTSTACK_HOST] wiimote_send_control: L2CAP not ready to send\n");
            return false;
        }

        // Convert DATA format (0xA2) to SET_REPORT format (0x52) for control channel
        // Some Wii U Pro Controllers are strict and reject DATA transactions on control channel
        uint8_t send_buf[64];
        memcpy(send_buf, data, len);
        if (send_buf[0] == 0xA2) {
            send_buf[0] = 0x52;  // SET_REPORT | OUTPUT
        }

        printf("[BTSTACK_HOST] wiimote_send_control via L2CAP: cid=0x%04X len=%d hdr=0x%02X\n",
               wiimote_conn.control_cid, len, send_buf[0]);
        uint8_t status = l2cap_send(wiimote_conn.control_cid, send_buf, len);
        if (status != ERROR_CODE_SUCCESS) {
            printf("[BTSTACK_HOST] wiimote_send_control: l2cap_send failed status=0x%02X\n", status);
        }
        return status == ERROR_CODE_SUCCESS;
    }

    // Fallback to HID Host when using_hid_host but no direct CID
    if (wiimote_conn.using_hid_host && wiimote_conn.hid_host_ready) {
        // Data format: first byte is 0x52 (SET_REPORT), second is report type+ID
        if (len < 2) return false;
        uint8_t report_id = data[1];
        uint16_t payload_len = len - 2;
        // hid_host_send_set_report stores a pointer — copy to static buffer for async send
        static uint8_t wiimote_hid_setreport_buf[80];
        if (payload_len > sizeof(wiimote_hid_setreport_buf)) return false;
        if (payload_len > 0) memcpy(wiimote_hid_setreport_buf, &data[2], payload_len);
        uint8_t status = hid_host_send_set_report(wiimote_conn.hid_host_cid, HID_REPORT_TYPE_OUTPUT,
                                                   report_id, wiimote_hid_setreport_buf, payload_len);
        if (status == ERROR_CODE_SUCCESS) {
            printf("[BTSTACK_HOST] wiimote_send_control: sent %d bytes via HID Host\n", len);
        }
        return status == ERROR_CODE_SUCCESS;
    }

    printf("[BTSTACK_HOST] wiimote_send_control: no control CID and HID Host not ready\n");
    return false;
}

// Get connection info for bthid driver matching (Classic or BLE)
bool btstack_classic_get_connection(uint8_t conn_index, btstack_classic_conn_info_t* info)
{
    if (!info) return false;

    // Check if this is a BLE connection (conn_index >= BLE_CONN_INDEX_OFFSET)
    if (conn_index >= BLE_CONN_INDEX_OFFSET) {
        uint8_t ble_index = conn_index - BLE_CONN_INDEX_OFFSET;
        if (ble_index >= MAX_BLE_CONNECTIONS) return false;

        ble_connection_t* conn = &hid_state.connections[ble_index];
        if (conn->handle == HCI_CON_HANDLE_INVALID) return false;

        info->active = true;
        memcpy(info->bd_addr, conn->addr, 6);
        strncpy(info->name, conn->name, sizeof(info->name) - 1);
        info->name[sizeof(info->name) - 1] = '\0';
        // BLE devices don't have class_of_device, set to zeros
        memset(info->class_of_device, 0, 3);
        // Use VID/PID from BLE manufacturer data (e.g., Switch 2)
        info->vendor_id = conn->vid;
        info->product_id = conn->pid;
        info->hid_ready = conn->hid_ready;
        info->is_ble = true;

        return true;
    }

    // Classic connection
    if (conn_index >= MAX_CLASSIC_CONNECTIONS) return false;

    classic_connection_t* conn = &classic_state.connections[conn_index];
    if (!conn->active) return false;

    info->active = conn->active;
    memcpy(info->bd_addr, conn->addr, 6);
    strncpy(info->name, conn->name, sizeof(info->name) - 1);
    info->name[sizeof(info->name) - 1] = '\0';
    memcpy(info->class_of_device, conn->class_of_device, 3);
    info->vendor_id = conn->vendor_id;
    info->product_id = conn->product_id;
    info->hid_ready = conn->hid_ready;
    info->is_ble = false;

    return true;
}

static uint8_t classic_link_count(void)
{
    uint8_t n = 0;
    for (int i = 0; i < MAX_CLASSIC_CONNECTIONS; i++) {
        if (classic_state.connections[i].active) n++;
    }
    return n;
}

// Get number of active connections (Classic + BLE)
uint8_t btstack_classic_get_connection_count(void)
{
    uint8_t count = 0;
    for (int i = 0; i < MAX_CLASSIC_CONNECTIONS; i++) {
        if (classic_state.connections[i].active) {
            count++;
        }
    }
    for (int i = 0; i < MAX_BLE_CONNECTIONS; i++) {
        if (hid_state.connections[i].handle != HCI_CON_HANDLE_INVALID) {
            count++;
        }
    }
    return count;
}

// ============================================================================
// DISCONNECT ALL
// ============================================================================

void btstack_host_disconnect_all_devices(void)
{
    printf("[BTSTACK_HOST] Disconnecting all devices...\n");

    for (int i = 0; i < MAX_CLASSIC_CONNECTIONS; i++) {
        classic_connection_t* c = &classic_state.connections[i];
        if (!c->active || !c->hid_cid) continue;
        // Drop the underlying ACL link, not just the HID profile. Closing
        // only HID (hid_host_disconnect) leaves the ACL up and some pads
        // (notably the DS4) hold their "connected to host" state past the
        // HID drop -- lightbar stays solid blue and the controller never
        // sleeps. Disconnecting the ACL via gap_disconnect() forces the
        // pad into its post-disconnect state where idle-sleep kicks in.
        hci_connection_t* hci_conn = hci_connection_for_bd_addr_and_type(
            c->addr, BD_ADDR_TYPE_ACL);
        if (hci_conn) {
            gap_disconnect(hci_conn->con_handle);
        } else {
            hid_host_disconnect(c->hid_cid);  // fallback
        }
    }
    for (int i = 0; i < MAX_BLE_CONNECTIONS; i++) {
        if (hid_state.connections[i].handle != HCI_CON_HANDLE_INVALID) {
            gap_disconnect(hid_state.connections[i].handle);
        }
    }

    // Clear reconnection state so we don't try to reconnect to cleared devices
    hid_state.has_last_connected = false;
    hid_state.reconnect_attempts = 0;
}

// ============================================================================
// BOND MANAGEMENT
// ============================================================================

void btstack_host_delete_all_bonds(void)
{
    printf("[BTSTACK_HOST] Deleting all Bluetooth bonds...\n");

#if !defined(BTSTACK_USE_CYW43) && !defined(BTSTACK_USE_ESP32) && !defined(BTSTACK_USE_NRF)
    // Erase BTstack flash banks to force clean re-initialization
    // This is more reliable than using BTstack's delete APIs when flash was corrupted
    btstack_erase_flash_banks();

    // Re-initialize the TLV context to pick up the erased banks
    const hal_flash_bank_t *flash_bank = pico_flash_bank_instance();
    btstack_tlv_flash_bank_init_instance(&btstack_tlv_flash_bank_context,
                                          flash_bank, NULL);
    printf("[BTSTACK_HOST] TLV re-initialized with clean flash banks\n");
#else
    // For CYW43/ESP32, use BTstack's standard APIs
    gap_delete_all_link_keys();
    printf("[BTSTACK_HOST] Classic BT link keys deleted\n");

    int ble_count = le_device_db_count();
    le_device_db_init();
    printf("[BTSTACK_HOST] BLE bonds deleted (was %d devices)\n", ble_count);
#endif

    // Also clear the last-connected record (RAM + JPLC flash tag) — otherwise
    // the periodic BLE reconnect loop resurrects a deleted bond on next boot
    // and burns the radio in doomed 10s gap_connect() attempts.
    hid_state.has_last_connected = false;
    memset(hid_state.last_connected_addr, 0, sizeof(hid_state.last_connected_addr));
    hid_state.last_connected_name[0] = '\0';
    {
        const btstack_tlv_t *tlv_impl = NULL;
        void *tlv_context = NULL;
        btstack_tlv_get_instance(&tlv_impl, &tlv_context);
        if (tlv_impl && tlv_impl->delete_tag) {
            tlv_impl->delete_tag(tlv_context, TLV_TAG_LAST_CONNECTED);
            printf("[BTSTACK_HOST] Last-connected record cleared\n");
        }
    }

    // Resume scanning. delete_all_bonds() disconnected every link and cleared the
    // reconnect target, leaving the host idle with nothing driving discovery — so a
    // freshly-unbonded controller put into pairing mode was never seen (you had to
    // reboot to pair again). Kick scanning back on so a re-pair works immediately.
    hid_state.state = BLE_STATE_IDLE;
    btstack_host_start_scan();

    printf("[BTSTACK_HOST] All bonds cleared. Devices will need to re-pair.\n");
}

// Enumerate persisted Classic BT link keys.
//
// The last-connected slot above is written only from SM_EVENT_PAIRING_COMPLETE /
// SM_EVENT_REENCRYPTION_COMPLETE — Security Manager events, which are BLE-only —
// so it can never describe a bonded Classic pad. Classic bonds live in the link
// key DB instead: hci_set_link_key_db() is called on both transports (the USB
// dongle path in setup_tlv_storage() above, and the CYW43 path in the SDK's
// btstack_cyw43.c), so gap_link_key_iterator_*() is the one accessor that works
// for both.
int btstack_host_list_classic_bonds(uint8_t addrs_out[][6], int max_count)
{
#ifdef ENABLE_CLASSIC
    if (!addrs_out || max_count <= 0) return 0;

    btstack_link_key_iterator_t it;
    if (!gap_link_key_iterator_init(&it)) {
        // iterator_init is optional in the btstack_link_key_db interface, and
        // gap_link_key_iterator_init() also returns 0 before HCI reaches the
        // working state. Report "no bonds" rather than failing the whole query.
        return 0;
    }

    int count = 0;
    bd_addr_t addr;
    link_key_t link_key;
    link_key_type_t type;
    while (count < max_count &&
           gap_link_key_iterator_get_next(&it, addr, link_key, &type)) {
        memcpy(addrs_out[count++], addr, 6);
    }
    gap_link_key_iterator_done(&it);

    // Don't leave key material on the stack — callers only ever want addresses.
    memset(link_key, 0, sizeof(link_key));
    return count;
#else
    (void)addrs_out;
    (void)max_count;
    return 0;
#endif
}

bool btstack_host_get_last_connected(uint8_t bd_addr_out[6], char name_out[48])
{
    if (!hid_state.has_last_connected) return false;
    bool nonzero = false;
    for (int i = 0; i < 6; i++) if (hid_state.last_connected_addr[i]) { nonzero = true; break; }
    if (!nonzero) return false;
    memcpy(bd_addr_out, hid_state.last_connected_addr, 6);
    strncpy(name_out, hid_state.last_connected_name, 47);
    name_out[47] = '\0';
    return true;
}

void btstack_host_forget_device(const uint8_t bd_addr[6])
{
    if (!hid_state.initialized) return;

    bd_addr_t addr;
    memcpy(addr, bd_addr, 6);

    printf("[BTSTACK_HOST] Forgetting device %02X:%02X:%02X:%02X:%02X:%02X\n",
           addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);

    // Disconnect if currently connected
    for (int i = 0; i < MAX_BLE_CONNECTIONS; i++) {
        if (hid_state.connections[i].handle != HCI_CON_HANDLE_INVALID &&
            memcmp(hid_state.connections[i].addr, addr, 6) == 0) {
            gap_disconnect(hid_state.connections[i].handle);
        }
    }

    // Remove BLE bond
    int bond_count = le_device_db_count();
    for (int i = 0; i < bond_count; i++) {
        int addr_type;
        bd_addr_t bond_addr;
        sm_key_t irk;
        le_device_db_info(i, &addr_type, bond_addr, irk);
        if (addr_type < 0) continue;
        if (memcmp(bond_addr, addr, 6) == 0) {
            le_device_db_remove(i);
            printf("[BTSTACK_HOST] Removed BLE bond at index %d\n", i);
            break;
        }
    }

    // Remove Classic link key
#ifdef ENABLE_CLASSIC
    gap_drop_link_key_for_bd_addr(addr);
#endif

    // Clear last-connected if it matches
    if (hid_state.has_last_connected &&
        memcmp(hid_state.last_connected_addr, addr, 6) == 0) {
        memset(hid_state.last_connected_addr, 0, 6);
        hid_state.has_last_connected = false;
        btstack_host_save_last_connected();  // Save the cleared state
    }
}
