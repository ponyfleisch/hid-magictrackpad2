#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/device.h>
#include <linux/hid.h>
#include <linux/input/mt.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/usb/input.h>

#define BT_VENDOR_ID_APPLE    0x004c
#define USB_DEVICE_ID_APPLE_MAGICTRACKPAD2    0x0265

static const struct hid_device_id magic_trackpads[] = {
        {HID_BLUETOOTH_DEVICE(BT_VENDOR_ID_APPLE,
                              USB_DEVICE_ID_APPLE_MAGICTRACKPAD2), .driver_data = 0},

        // USB is broken right now.

//	{ HID_USB_DEVICE(USB_VENDOR_ID_APPLE,
//		USB_DEVICE_ID_APPLE_MAGICTRACKPAD2), .driver_data = 0 },
        {}
};

// host controlled haptic feedback for the click/force clicks.
// reverse engineered. values at index 4, 7 and 15 change the click behaviour
static __u8 button_down[] = {0xF2, 0x53, 0x01, 0x17, 0x78, 0x02, 0x06, 0x24, 0x30, 0x06, 0x01, 0x06, 0x18, 0x48, 0x12};
static __u8 button_force[] = {0xF2, 0x53, 0x01, 0x1c, 0x78, 0x02, 0x0A, 0x24, 0x30, 0x06, 0x01, 0x0d, 0x18, 0x48, 0x12};
static __u8 button_up[] = {0xF2, 0x53, 0x01, 0x14, 0x78, 0x02, 0x00, 0x24, 0x30, 0x06, 0x01, 0x00, 0x18, 0x48, 0x12};

MODULE_DEVICE_TABLE(hid, magic_trackpads
);

#define MAX_FINGERS        16
#define MAX_FINGER_ORIENTATION    16384

#define PRESSURE_CLICK_THRESHOLD 50
#define PRESSURE_FORCE_THRESHOLD 140

/* list of device capability bits */
#define HAS_INTEGRATED_BUTTON    1

/* logical signal quality */
#define SN_PRESSURE    45        /* pressure signal-to-noise ratio */
#define SN_WIDTH    25        /* width signal-to-noise ratio */
#define SN_COORD    250        /* coordinate signal-to-noise ratio */
#define SN_ORIENT    10        /* orientation signal-to-noise ratio */

struct bcm5974_param {
    int snratio;        /* signal-to-noise ratio */
    int min;        /* device minimum reading */
    int max;        /* device maximum reading */
};
struct bt_data {
    u8 unknown1;        /* constant */
    u8 button;        /* left button */
    u8 rel_x;        /* relative x coordinate */
    u8 rel_y;        /* relative y coordinate */
};

struct tp_finger {
    u8 abs_x;            /* absolute x coodinate */
    u8 abs_x_y;            /* absolute x,y coodinate */
    u8 abs_y[2];        /* absolute y coodinate */
    u8 touch_major;        /* touch area, major axis */
    u8 touch_minor;        /* touch area, minor axis */
    u8 size;            /* tool area, size */
    u8 pressure;        /* pressure on forcetouch touchpad */
    u8 orientation_origin;    /* orientation and id */
} __attribute__((packed, aligned(2)));

struct bcm5974_config {
    int ansi, iso, jis;    /* the product id of this device */
    int caps;        /* device capability bitmask */
    int bt_ep;        /* the endpoint of the button interface */
    int bt_datalen;        /* data length of the button interface */
    struct bcm5974_param p;    /* finger pressure limits */
    struct bcm5974_param w;    /* finger width limits */
    struct bcm5974_param x;    /* horizontal limits */
    struct bcm5974_param y;    /* vertical limits */
    struct bcm5974_param o;    /* orientation limits */
};

struct bcm5974 {
    struct input_dev *input;    /* input dev */
    struct bcm5974_config cfg;    /* device configuration */
    struct mutex pm_mutex;        /* serialize access to open/suspend */
    int opened;            /* 1: opened, 0: closed */
    struct urb *bt_urb;        /* button usb request block */
    struct bt_data *bt_data;    /* button transferred data */
    struct urb *tp_urb;        /* trackpad usb request block */
    u8 *tp_data;            /* trackpad transferred data */
    const struct tp_finger *index[MAX_FINGERS];    /* finger index data */
    struct input_mt_pos pos[MAX_FINGERS];        /* position array */
    int slots[MAX_FINGERS];                /* slot assignments */
};

static const struct bcm5974_config trackpad_config = {
    USB_DEVICE_ID_APPLE_MAGICTRACKPAD2,
    USB_DEVICE_ID_APPLE_MAGICTRACKPAD2,
    USB_DEVICE_ID_APPLE_MAGICTRACKPAD2,
    HAS_INTEGRATED_BUTTON,
    0, sizeof(struct bt_data),
    {SN_PRESSURE, 0, 300},
    {SN_WIDTH, 0, 2048},
    {SN_COORD, -3678, 3934},
    {SN_COORD, -2479, 2586},
    {SN_ORIENT, -MAX_FINGER_ORIENTATION, MAX_FINGER_ORIENTATION}
};

static int probe(struct hid_device *hdev, const struct hid_device_id *id) {
    struct bcm5974 *device;
    int ret;

    device = devm_kzalloc(&hdev->dev, sizeof(*device), GFP_KERNEL);

    if (device == NULL) {
        hid_err(hdev, "can't alloc trackpad descriptor\n");
        return -ENOMEM;
    }

    hid_set_drvdata(hdev, device);
    ret = hid_parse(hdev);
    if (ret) {
        hid_err(hdev, "trackpad hid parse failed\n");
        return ret;
    }

    ret = hid_hw_start(hdev, HID_CONNECT_DEFAULT);
    if (ret) {
        hid_err(hdev, "trackpad hw start failed\n");
        return ret;
    }

    if (!device->input) {
        hid_err(hdev, "trackpad input not registered\n");
        ret = -ENOMEM;
        goto err_stop_hw;
    }

    hid_register_report(hdev, HID_INPUT_REPORT, 0x31);

    return 0;

    err_stop_hw:
    hid_hw_stop(hdev);
    return ret;
}

static inline void bup(struct hid_device *hdev) {
    hid_hw_output_report(hdev, button_up, sizeof(button_up));
}

static inline void bdown(struct hid_device *hdev) {
    hid_hw_output_report(hdev, button_down, sizeof(button_down));
}

static inline void bforce(struct hid_device *hdev) {
    hid_hw_output_report(hdev, button_force, sizeof(button_force));
}


/* convert 16-bit little endian to signed integer */
static inline int raw2int(__le16 x) {
    return (signed short) le16_to_cpu(x);
}


static int raw_event(struct hid_device *hdev, struct hid_report *report, u8 *data, int size) {
    static char status = 0;
    __u8 pressure = 0;
    __u8 id = 0;
    int active_ids[MAX_FINGERS + 1] = {0};
    int current_slot = -1;
    int i = 0, j = 0;
    s16 tmp_x;
    s32 tmp_y;
    struct bcm5974 *device = hid_get_drvdata(hdev);

    const struct tp_finger *finger;

    // message with only a timestamp means all fingers have been lifted
    if (size <= 4) {
        for (i = 0; i < MAX_FINGERS; i++) {
            if (device->slots[i] != 0) {
                input_mt_slot(device->input, i);
                input_report_abs(device->input, ABS_MT_TRACKING_ID, -1);
                device->slots[i] = 0;
                input_mt_sync_frame(device->input);
            }
        }
        input_sync(device->input);
        return 0;
    } else {
        for (i = 0; i < (size - 4) / 9; i++) {
            finger = (const struct tp_finger *) (data + 4 + (i * 9));
            id = finger->orientation_origin & 0x0F;
            active_ids[id] = 1;

            current_slot = -1;

            // do we already track this finger?
            for (j = 0; j < MAX_FINGERS; j++) {
                if (device->slots[j] == id) {
                    current_slot = j;
                    input_mt_slot(device->input, current_slot);
                    break;
                }
            }

            // new finger?
            if (current_slot == -1) {
                for (j = 0; j < MAX_FINGERS; j++) {
                    if (device->slots[j] == 0) {
                        device->slots[j] = id;
                        current_slot = j;
                        input_mt_slot(device->input, current_slot);
                        input_report_abs(device->input, ABS_MT_TRACKING_ID, id);
                        break;
                    }
                }
            }

            tmp_x = (s16)((le16_to_cpu(*((__le16 *) finger)) & 0x1fff) << 3) >> 3;
            tmp_y = -(s32)(((s32) le32_to_cpu(*((__le32 *) finger))) << 6) >> 19;

            // for the first finger, we also report non-multitrack data
            if (i == 0) {
                input_report_abs(device->input, ABS_X, tmp_x);
                input_report_abs(device->input, ABS_Y, tmp_y);

                // i'm not sure if that is a synaptics issue, but there is no movement
                // if the pressure is below ~30
                input_report_abs(device->input, ABS_PRESSURE, finger->pressure + 30);

                // no idea if this is correct
                input_report_abs(device->input, ABS_TOOL_WIDTH, finger->size);
            }

            // save the greatest pressure for later (button) use
            if (finger->pressure > pressure) {
                pressure = finger->pressure;
            }

            input_mt_report_slot_state(device->input, MT_TOOL_FINGER, true);
            input_report_abs(device->input, ABS_MT_PRESSURE, finger->pressure);
            input_report_abs(device->input, ABS_MT_TOUCH_MAJOR,
                             raw2int(finger->touch_major) << 2);
            input_report_abs(device->input, ABS_MT_TOUCH_MINOR,
                             raw2int(finger->touch_minor) << 2);

            input_report_abs(device->input, ABS_MT_ORIENTATION,
                             MAX_FINGER_ORIENTATION - ((finger->orientation_origin & 0xf0) << 6));

            input_report_abs(device->input, ABS_MT_POSITION_X, tmp_x);
            input_report_abs(device->input, ABS_MT_POSITION_Y, tmp_y);
        }
    }

    // have any fingers been lifted?
    for (i = 0; i < MAX_FINGERS; i++) {
        if (device->slots[i] != 0) {
            if (active_ids[device->slots[i]] == 0) {
                device->slots[i] = 0;
                input_mt_slot(device->input, i);
                input_report_abs(device->input, ABS_MT_TRACKING_ID, -1);
            }
        }
    }

    input_sync(device->input);

    if (pressure > PRESSURE_CLICK_THRESHOLD && pressure < PRESSURE_FORCE_THRESHOLD && status == 0) {
        bdown(hdev);
        status = 1;
        input_report_key(device->input, BTN_LEFT, 1);
    } else if (pressure >= PRESSURE_FORCE_THRESHOLD && status == 1) {
        bforce(hdev);
        status = 2;
        input_report_key(device->input, BTN_LEFT, 0);
        input_report_key(device->input, BTN_MIDDLE, 1);
    } else if (pressure > PRESSURE_CLICK_THRESHOLD && pressure < PRESSURE_FORCE_THRESHOLD && status == 2) {
        bup(hdev);
        status = 3;
        input_report_key(device->input, BTN_LEFT, 0);
        input_report_key(device->input, BTN_MIDDLE, 0);
    } else if (pressure <= PRESSURE_CLICK_THRESHOLD && status == 1) {
        bup(hdev);
        status = 0;
        input_report_key(device->input, BTN_LEFT, 0);
    } else if (pressure <= PRESSURE_CLICK_THRESHOLD && status == 3) {
        status = 0;
    }

    return 0;
}

static int input_mapping(struct hid_device *hdev,
                         struct hid_input *hi, struct hid_field *field,
                         struct hid_usage *usage, unsigned long **bit, int *max) {
    struct bcm5974 *device = hid_get_drvdata(hdev);

    if (!device->input) {
        device->input = hi->input;
    }

    return 0;
}

static void set_abs(struct input_dev *input, unsigned int code,
                    const struct bcm5974_param *p) {
    int fuzz = p->snratio ? (p->max - p->min) / p->snratio : 0;
    input_set_abs_params(input, code, p->min, p->max, fuzz, 0);
}


static int setup_input(struct input_dev *input_dev, struct hid_device *hdev) {
    int size;

    // reverse engineered.
    __u8 m1[] = {0xF1, 0x01, 0xDB}, // unknown purpose.
            enableHostClick[] = {0xF2, 0x21, 0x01}, // enables host clicks/disables autonomous clicks
            m5[] = {0xF1, 0x01, 0xC8}, // unknown purpose.
            enableMultiTrackMode[] = {0xF1, 0x02, 0x01},
            enableEmptyFingerReport[] = {0xF1, 0xC8, 0x09};

    // if we leave the bt device name, the default synaptics/X11
    // config will not apply the special rules for apple devices.
    input_dev->name = "Apple Magic Trackpad 2";

    __clear_bit(EV_REL, input_dev->evbit);
    __clear_bit(REL_X, input_dev->relbit);
    __clear_bit(REL_Y, input_dev->relbit);

    __set_bit(EV_ABS, input_dev->evbit);
    __clear_bit(BTN_RIGHT, input_dev->keybit);

    // if we enable this, the X11 synaptics driver
    // will set ClickAction to 1 1 0 rather than 1 3 0
    //__set_bit(BTN_MIDDLE, input_dev->keybit);

    __set_bit(BTN_MOUSE, input_dev->keybit);
    __set_bit(BTN_TOOL_FINGER, input_dev->keybit);
    __set_bit(BTN_TOOL_DOUBLETAP, input_dev->keybit);
    __set_bit(BTN_TOOL_TRIPLETAP, input_dev->keybit);
    __set_bit(BTN_TOOL_QUADTAP, input_dev->keybit);
    __set_bit(BTN_TOOL_QUINTTAP, input_dev->keybit);
    __set_bit(BTN_TOUCH, input_dev->keybit);
    __set_bit(INPUT_PROP_POINTER, input_dev->propbit);
    __set_bit(INPUT_PROP_BUTTONPAD, input_dev->propbit);

    /* for synaptics only */
    input_set_abs_params(input_dev, ABS_PRESSURE, 0, 256, 0, 0);
    input_set_abs_params(input_dev, ABS_TOOL_WIDTH, 0, 16, 0, 0);

    /* finger touch area */
    set_abs(input_dev, ABS_MT_TOUCH_MAJOR, &trackpad_config.w);
    set_abs(input_dev, ABS_MT_TOUCH_MINOR, &trackpad_config.w);
    /* finger orientation */
    set_abs(input_dev, ABS_MT_ORIENTATION, &trackpad_config.o);
    /* finger position */
    set_abs(input_dev, ABS_MT_POSITION_X, &trackpad_config.x);
    set_abs(input_dev, ABS_MT_POSITION_Y, &trackpad_config.y);

    __set_bit(EV_KEY, input_dev->evbit);
    __set_bit(BTN_LEFT, input_dev->keybit);

    if (trackpad_config.caps & HAS_INTEGRATED_BUTTON)
        __set_bit(INPUT_PROP_BUTTONPAD, input_dev->propbit);

    input_mt_init_slots(input_dev, MAX_FINGERS, INPUT_MT_POINTER);


    input_set_abs_params(input_dev, ABS_X, trackpad_config.x.min,
                         trackpad_config.x.max, 4, 0);
    input_set_abs_params(input_dev, ABS_Y, trackpad_config.y.min,
                         trackpad_config.y.max, 4, 0);
    input_set_abs_params(input_dev, ABS_MT_POSITION_X,
                         trackpad_config.x.min,
                         trackpad_config.x.max, 4, 0);
    input_set_abs_params(input_dev, ABS_MT_POSITION_Y,
                         trackpad_config.y.min,
                         trackpad_config.y.max, 4, 0);


    // hardware device initialisation. probably doesn't belong here.
    size = hid_hw_raw_request(hdev,
                              m1[0], m1,
                              sizeof(m1), HID_FEATURE_REPORT, HID_REQ_SET_REPORT);
    size = hid_hw_raw_request(hdev,
                              enableHostClick[0],
                              enableHostClick,
                              sizeof(enableHostClick),
                              HID_FEATURE_REPORT,
                              HID_REQ_SET_REPORT
    );
    size = hid_hw_raw_request(hdev,
                              m5[0], m5,
                              sizeof(m5), HID_FEATURE_REPORT, HID_REQ_SET_REPORT);
    size = hid_hw_raw_request(hdev,
                              enableMultiTrackMode[0],
                              enableMultiTrackMode,
                              sizeof(enableMultiTrackMode),
                              HID_FEATURE_REPORT,
                              HID_REQ_SET_REPORT
    );
    size = hid_hw_raw_request(hdev,
                              enableEmptyFingerReport[0],
                              enableEmptyFingerReport,
                              sizeof(enableEmptyFingerReport),
                              HID_FEATURE_REPORT,
                              HID_REQ_SET_REPORT
    );

    return 0;
}

static int input_configured(struct hid_device *hdev, struct hid_input *hi) {
    struct bcm5974 *device = hid_get_drvdata(hdev);
    int ret;

    ret = setup_input(device->input, hdev);
    if (ret) {
        hid_err(hdev, "trackpad setup input failed (%d)\n", ret);
        /* clean msc->input to notify probe() of the failure */
        device->input = NULL;
        return ret;
    }

    return 0;
}

static struct hid_driver trackpad_driver = {
        .name = "magictrackpad",
        .id_table = magic_trackpads,
        .probe = probe,
        .raw_event = raw_event,
        .input_mapping = input_mapping,
        .input_configured = input_configured,
};
module_hid_driver(trackpad_driver);

MODULE_LICENSE("GPL");
