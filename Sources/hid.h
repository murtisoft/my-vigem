#ifndef HID_H
#define HID_H

#include <wtypes.h>

struct hid_device_info;

struct hid_device_info
{
    LPTSTR instance_path;
    LPTSTR container_instance_path;
    LPTSTR symlink;
    LPTSTR description;

    struct hid_device_info *next;
};

struct hid_device
{
    struct hid_device_info *device_info;

    HANDLE handle;

    BOOLEAN read_pending;
    USHORT input_report_size;
    USHORT output_report_size;
    USHORT feature_report_size;
    BYTE *input_buffer;
    BYTE *output_buffer;
    BYTE *feature_buffer;

    OVERLAPPED input_ol;
};

GUID hid_get_interface_guid();
struct hid_device_info *hid_enumerate(const LPTSTR *path_filters);
BOOLEAN hid_reenable_device(struct hid_device_info *device_info);
// Turns the real pad off: disables the PARENT devnode of its HID device (container_instance_path) and enables
// it again, so the pad drops its connection and stays off until the user wakes it. Only uses the stored paths,
// so a clone is fine after the device was closed. Returns FALSE if the devnode could not be re-enabled.
BOOLEAN hid_power_off_device(struct hid_device_info *device_info);
void hid_enable_disabled_devices(const LPTSTR *path_filters);
BOOLEAN check_vendor_and_product(struct hid_device_info *device_info, USHORT vendor_id, USHORT product_id);
struct hid_device_info *hid_clone_device_info(struct hid_device_info *device_info);
void hid_free_device_info(struct hid_device_info *device_info);
struct hid_device *hid_open_device(struct hid_device_info *device_info, BOOLEAN access_rw, BOOLEAN shared);
INT hid_get_input_report(struct hid_device *device, DWORD timeout);
INT hid_send_output_report(struct hid_device *device, const void *data, size_t length);
INT hid_send_feature_report(struct hid_device *device, const void *data, size_t length);
// Liveness probe: sends the given feature report through a private buffer (safe next to the rumble thread).
// Returns FALSE if the device no longer accepts it (e.g. the pad switched off but Windows kept the devnode).
BOOLEAN hid_probe_feature(struct hid_device *device, const void *data, size_t length);
// Read a feature report by ID into out (report ID is out[0]). Returns the byte count copied, or -1 on failure (see GetLastError).
// Log the device's HID descriptor summary (attributes, report sizes, button/value caps per report type) with debug_log.
void hid_describe_device(struct hid_device *device);
INT hid_get_feature_report(struct hid_device *device, BYTE report_id, void *out, size_t out_size);
void hid_close_device(struct hid_device *device);
void hid_free_device(struct hid_device *device);

#endif /* HID_H */
