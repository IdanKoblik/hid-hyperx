#include "linux/container_of.h"
#include "linux/gfp_types.h"
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/hid.h>
#include <linux/usb.h>
#include <linux/device.h>

#define HYPERX_CLOUD_2_WIRELESS_VENDOR 0x03f0
#define HYPERX_CLOUD_2_WIRELESS_PRODUCT 0x0995

#define HYPERX_PREFIX "HyperX "
#define HYPERX_PREFIX_LEN strlen(HYPERX_PREFIX)

#define HYPERX_HEADSET_BATTERY_TIMEOUT_MS	3000

struct hyperx_device {
    struct hid_device *hdev;

    struct power_supply_desc battery_desc;
    struct power_supply *battery;

    struct delayed_work battery_work;
    spinlock_t lock;

    uint8_t battery_capacity;

    bool is_connected;
    bool is_charging;
};

static void hyperx_headset_set_wireless_status(struct hid_device *hdev, bool connected) {
	struct usb_interface *intf;
	if (!hid_is_usb(hdev))
		return;

	intf = to_usb_interface(hdev->dev.parent);
	usb_set_wireless_status(intf, connected ? USB_WIRELESS_STATUS_CONNECTED : USB_WIRELESS_STATUS_DISCONNECTED);
}

static enum power_supply_property hyperx_headset_battery_props[] = {
	POWER_SUPPLY_PROP_MODEL_NAME,
	POWER_SUPPLY_PROP_MANUFACTURER,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_SCOPE,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_CAPACITY_LEVEL,
};

#define HYPERX_CLOUD_2_WIRELESS_REPORT_LEN 62
#define HYPERX_CLOUD_2_WIRELESS_RESPONSE_LEN 5

#define HYPERX_REPORT_ID 0x66
#define HYPERX_ALIAS 0x0d
#define HYPERX_CMD_BATTERY_STATUS 0x89

static const char cloud_2_wireless_battery_request[HYPERX_CLOUD_2_WIRELESS_REPORT_LEN] = { HYPERX_REPORT_ID , HYPERX_CMD_BATTERY_STATUS };

static int hyperx_headset_request_battery(struct hid_device *hdev, const char *request, size_t len) {
    u8 *write_buf = kmemdup(request, len, GFP_KERNEL);
	if (!write_buf)
		return -ENOMEM;

	hid_dbg(hdev, "Sending battery request report");
	int ret = hid_hw_raw_request(hdev, request[0], write_buf, len, HID_OUTPUT_REPORT, HID_REQ_SET_REPORT);
	if (ret < (int)len) {
		hid_err(hdev, "hid_hw_raw_request() failed with %d\n", ret);
		ret = -ENODATA;
	}

	kfree(write_buf);
	return ret;
}

static void hyperx_headset_fetch_battery(struct hid_device *hdev) {
    int ret = 0;
    if (hdev->product == HYPERX_CLOUD_2_WIRELESS_PRODUCT)
        ret = hyperx_headset_request_battery(hdev, cloud_2_wireless_battery_request, sizeof(cloud_2_wireless_battery_request));

    if (ret < 0)
        hid_dbg(hdev, "Battery query failed (err: %d)\n", ret);
}

static void hyperx_headset_battery_timer_tick(struct work_struct *work) {
	struct hyperx_device *sd = container_of(work, struct hyperx_device, battery_work.work);
	struct hid_device *hdev = sd->hdev;

	hyperx_headset_fetch_battery(hdev);
}

static int battery_capacity_to_level(int capacity) {
	if (capacity >= 50)
		return POWER_SUPPLY_CAPACITY_LEVEL_NORMAL;

    if (capacity >= 20)
		return POWER_SUPPLY_CAPACITY_LEVEL_LOW;

    return POWER_SUPPLY_CAPACITY_LEVEL_CRITICAL;
}

static int hyperx_headset_battery_get_property(struct power_supply *psy, enum power_supply_property psp, union power_supply_propval *val) {
	struct hyperx_device *device = power_supply_get_drvdata(psy);
	int ret = 0;

	switch (psp) {
	case POWER_SUPPLY_PROP_MODEL_NAME:
		val->strval = device->hdev->name;
		while (!strncmp(val->strval, HYPERX_PREFIX, HYPERX_PREFIX_LEN))
			val->strval += HYPERX_PREFIX_LEN;
		break;
	case POWER_SUPPLY_PROP_MANUFACTURER:
		val->strval = "HyperX";
		break;
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = 1;
		break;
	case POWER_SUPPLY_PROP_STATUS:
		if (device->is_connected) {
			val->intval = device->is_charging ?
				POWER_SUPPLY_STATUS_CHARGING :
				POWER_SUPPLY_STATUS_DISCHARGING;
		} else
			val->intval = POWER_SUPPLY_STATUS_UNKNOWN;
		break;
	case POWER_SUPPLY_PROP_SCOPE:
		val->intval = POWER_SUPPLY_SCOPE_DEVICE;
		break;
	case POWER_SUPPLY_PROP_CAPACITY:
		val->intval = device->battery_capacity;
		break;
	case POWER_SUPPLY_PROP_CAPACITY_LEVEL:
		val->intval = battery_capacity_to_level(device->battery_capacity);
		break;
	default:
		ret = -EINVAL;
		break;
	}
	return ret;
}

static int hyperx_headset_battery_register(struct hyperx_device *device) {
	static atomic_t battery_no = ATOMIC_INIT(0);

    device->battery_desc.type = POWER_SUPPLY_TYPE_BATTERY;
	device->battery_desc.properties = hyperx_headset_battery_props;
	device->battery_desc.num_properties = ARRAY_SIZE(hyperx_headset_battery_props);
	device->battery_desc.get_property = hyperx_headset_battery_get_property;
	device->battery_desc.use_for_apm = 0;
	unsigned long n = atomic_inc_return(&battery_no) - 1;

    device->battery_desc.name = devm_kasprintf(&device->hdev->dev, GFP_KERNEL, "hyperx_headset_battery_%ld", n);
	if (!device->battery_desc.name)
		return -ENOMEM;

    hyperx_headset_set_wireless_status(device->hdev, false);
	device->battery_capacity = 100;
	device->is_charging = false;

    struct power_supply_config battery_cfg = { .drv_data = device, };
    device->battery = devm_power_supply_register(&device->hdev->dev, &device->battery_desc, &battery_cfg);

    if (IS_ERR(device->battery)) {
		int ret = PTR_ERR(device->battery);
		hid_err(device->hdev, "%s:power_supply_register failed with error %d\n", __func__, ret);
		return ret;
	}

    power_supply_powers(device->battery, &device->hdev->dev);

    INIT_DELAYED_WORK(&device->battery_work, hyperx_headset_battery_timer_tick);
    hyperx_headset_fetch_battery(device->hdev);

    return 0;
}

static uint8_t hyperx_headset_map_capacity(uint8_t capacity) {
	uint8_t max = 100;
    uint8_t min = 0;

    if (capacity >= max)
		return 100;
	if (capacity <= min)
		return 0;
	return (capacity - min) * 100 / (max - min);
}

static int hyperx_raw_event(struct hid_device *hdev, struct hid_report *report, u8 *data, int size) {
    hid_info(hdev, "Raw: %*ph\n", size, data);

    struct hyperx_device *device = hid_get_drvdata(hdev);
    if (!device)
        return -ENOMEM;

    int capacity = device->battery_capacity;
	bool connected = device->is_connected;
	bool charging = device->is_charging;
    unsigned long flags;

    if (hdev->product == HYPERX_CLOUD_2_WIRELESS_PRODUCT) {
        hid_dbg(device->hdev, "Parsing raw event for HyperX cloud 2 wireless headset (%*ph)\n", size, data);
        if (size < HYPERX_CLOUD_2_WIRELESS_RESPONSE_LEN) {
            if (!delayed_work_pending(&device->battery_work))
                goto request_battery;
            return 0;
        }

        if (data[0] == HYPERX_REPORT_ID && (data[1] == HYPERX_ALIAS || data[1] == HYPERX_CMD_BATTERY_STATUS)) {
            if (data[2] != 0x00 || data[3] != 0x00) {
                connected = true;
                charging = false;
                capacity = hyperx_headset_map_capacity(data[4]);
            } else {
                connected = false;
                charging = false;
            }
        }
    }

    if (connected != device->is_connected) {
        hid_dbg(device->hdev,
                "Connected status changed from %sconnected to %sconnected\n",
                device->is_connected ? "" : "not ",
                connected ? "" : "not "
        );

        device->is_connected = connected;
        hyperx_headset_set_wireless_status(hdev, connected);
    }

    if (capacity != device->battery_capacity) {
        hid_dbg(device->hdev,
                "Battery capacity changed from %d%% to %d%%\n",
                device->battery_capacity, capacity
        );

        device->battery_capacity = capacity;
        power_supply_changed(device->battery);
    }

    if (charging != device->is_charging) {
        hid_dbg(device->hdev,
                "Battery charging status changed from %scharging to %scharging\n",
                device->is_charging ? "" : "not ",
                charging ? "" : "not "
        );

        device->is_charging = charging;
        power_supply_changed(device->battery);
    }

request_battery:
	spin_lock_irqsave(&device->lock, flags);
	if (device->is_connected)
		schedule_delayed_work(&device->battery_work, msecs_to_jiffies(HYPERX_HEADSET_BATTERY_TIMEOUT_MS));

    spin_unlock_irqrestore(&device->lock, flags);
    return 0;
}

static int hyperx_probe(struct hid_device *hdev, const struct hid_device_id *id) {
    struct hyperx_device *device = devm_kzalloc(&hdev->dev, sizeof(*device), GFP_KERNEL);
    if (!device)
        return -ENOMEM;

    hid_set_drvdata(hdev, device);
    device->hdev = hdev;

    int ret = hid_parse(device->hdev);
    if (ret)
        return ret;

    spin_lock_init(&device->lock);

    ret = hid_hw_start(hdev, HID_CONNECT_DEFAULT);
    if (ret)
        return ret;

    ret = hid_hw_open(hdev);
    if (ret)
        return ret;

    if (hyperx_headset_battery_register(device) < 0)
        hid_err(device->hdev, "Failed to register battery for headset\n");

    return 0;
}

static void hyperx_remove(struct hid_device *hdev) {
    struct hyperx_device *device = hid_get_drvdata(hdev);
    unsigned long flags;

	spin_lock_irqsave(&device->lock, flags);
    device->is_connected = false;
    spin_unlock_irqrestore(&device->lock, flags);

	cancel_delayed_work_sync(&device->battery_work);

    hid_hw_close(hdev);
    hid_hw_stop(hdev);
}

static const struct hid_device_id hyperx_devices[] = {
    {HID_USB_DEVICE(HYPERX_CLOUD_2_WIRELESS_VENDOR, HYPERX_CLOUD_2_WIRELESS_PRODUCT)},
    {}
};

MODULE_DEVICE_TABLE(hid, hyperx_devices);

static struct hid_driver hyperx_driver = {
	.name = "hyperx",
	.id_table = hyperx_devices,
	.probe = hyperx_probe,
	.remove = hyperx_remove,
	.raw_event = hyperx_raw_event,
};
module_hid_driver(hyperx_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Idan Koblik <me@idank.dev>");
MODULE_DESCRIPTION("HID driver for Hyperx devices");
