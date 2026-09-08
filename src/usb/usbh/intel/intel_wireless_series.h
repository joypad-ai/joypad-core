// Intel Wireless Series USB base station (8086:C013).
#ifndef INTEL_WIRELESS_SERIES_H
#define INTEL_WIRELESS_SERIES_H

#include "tusb.h"
#if CFG_TUH_ENABLED
#include "host/usbh_pvt.h"
extern const usbh_class_driver_t usbh_intel_wireless_series_driver;
void intel_wireless_series_task(void);
void intel_wireless_series_disconnect(uint8_t dev_addr);
#endif
#endif
