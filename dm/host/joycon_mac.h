// Mac-only Joy-Con HID driver: handles Left Joy-Con (PID 0x2006) and Right Joy-Con (PID 0x2007)
// directly via IOHIDManager, sending initialization subcommands and reading Report 0x30 at 60 Hz.
// Synthesizes a unified JoyDual controller for Player 1 and feeds dual 6-axis IMU sensors.
#pragma once
#import <IOKit/hid/IOHIDManager.h>
#import <GameController/GCSyntheticDeviceKeys.h>
#include "dm_input.h"
#include "hle.h"
#include <stdatomic.h>
#include <math.h>
#include <pthread.h>
#include "hle_haptics.h"

typedef struct joycon_device {
    IOHIDDeviceRef hid;
    int is_left; // 1 = Left, 0 = Right
    uint8_t report[512];
    uint8_t rumble_counter;
    int was_rumbling;
    struct joycon_device *next;
} joycon_device;

static IOHIDManagerRef g_joycon_manager;
static joycon_device *g_joycon_devices;
static pthread_mutex_t g_joycon_dev_lock = PTHREAD_MUTEX_INITIALIZER;
static joycon_device *g_jc_left_dev = NULL;
static joycon_device *g_jc_right_dev = NULL;
static _Atomic int g_joycon_left_connected, g_joycon_right_connected;
static _Atomic uint64_t g_jc_left_buttons, g_jc_right_buttons;
static _Atomic int32_t g_jc_left_lx, g_jc_left_ly, g_jc_right_rx, g_jc_right_ry;

int dm_joycon_is_dual(void) {
    return atomic_load(&g_joycon_left_connected) && atomic_load(&g_joycon_right_connected);
}

int dm_joycon_count(void) {
    return (atomic_load(&g_joycon_left_connected) ? 1 : 0) + (atomic_load(&g_joycon_right_connected) ? 1 : 0);
}

int dm_joycon_has_haptics(void) {
    return atomic_load(&g_joycon_left_connected) || atomic_load(&g_joycon_right_connected);
}

static inline uint16_t jc_encode_hf(float freq) {
    if (freq < 82.0f) freq = 82.0f;
    if (freq > 1253.0f) freq = 1253.0f;
    uint8_t nf = (uint8_t)fminf(fmaxf(log2f(freq / 10.0f) * 32.0f, 0.0f), 255.0f);
    return (uint16_t)((nf - 0x60) * 4);
}

static inline uint8_t jc_encode_lf(float freq) {
    if (freq < 41.0f) freq = 41.0f;
    if (freq > 626.0f) freq = 626.0f;
    uint8_t nf = (uint8_t)fminf(fmaxf(log2f(freq / 10.0f) * 32.0f, 0.0f), 255.0f);
    return (uint8_t)(nf - 0x40);
}

static const struct { float amp; uint8_t code; } jc_hf_amp[101] = {
    {0.0f, 0x0}, {0.01f, 0x2}, {0.012f, 0x4}, {0.014f, 0x6}, {0.017f, 0x8},
    {0.02f, 0x0a}, {0.024f, 0x0c}, {0.028f, 0x0e}, {0.033f, 0x10}, {0.04f, 0x12},
    {0.047f, 0x14}, {0.056f, 0x16}, {0.067f, 0x18}, {0.08f, 0x1a}, {0.095f, 0x1c},
    {0.112f, 0x1e}, {0.117f, 0x20}, {0.123f, 0x22}, {0.128f, 0x24}, {0.134f, 0x26},
    {0.14f, 0x28}, {0.146f, 0x2a}, {0.152f, 0x2c}, {0.159f, 0x2e}, {0.166f, 0x30},
    {0.173f, 0x32}, {0.181f, 0x34}, {0.189f, 0x36}, {0.198f, 0x38}, {0.206f, 0x3a},
    {0.215f, 0x3c}, {0.225f, 0x3e}, {0.23f, 0x40}, {0.235f, 0x42}, {0.24f, 0x44},
    {0.245f, 0x46}, {0.251f, 0x48}, {0.256f, 0x4a}, {0.262f, 0x4c}, {0.268f, 0x4e},
    {0.273f, 0x50}, {0.279f, 0x52}, {0.286f, 0x54}, {0.292f, 0x56}, {0.298f, 0x58},
    {0.305f, 0x5a}, {0.311f, 0x5c}, {0.318f, 0x5e}, {0.325f, 0x60}, {0.332f, 0x62},
    {0.34f, 0x64}, {0.347f, 0x66}, {0.355f, 0x68}, {0.362f, 0x6a}, {0.37f, 0x6c},
    {0.378f, 0x6e}, {0.387f, 0x70}, {0.395f, 0x72}, {0.404f, 0x74}, {0.413f, 0x76},
    {0.422f, 0x78}, {0.431f, 0x7a}, {0.44f, 0x7c}, {0.45f, 0x7e}, {0.46f, 0x80},
    {0.47f, 0x82}, {0.48f, 0x84}, {0.491f, 0x86}, {0.501f, 0x88}, {0.512f, 0x8a},
    {0.524f, 0x8c}, {0.535f, 0x8e}, {0.547f, 0x90}, {0.559f, 0x92}, {0.571f, 0x94},
    {0.584f, 0x96}, {0.596f, 0x98}, {0.609f, 0x9a}, {0.623f, 0x9c}, {0.636f, 0x9e},
    {0.65f, 0xa0}, {0.665f, 0xa2}, {0.679f, 0xa4}, {0.694f, 0xa6}, {0.709f, 0xa8},
    {0.725f, 0xaa}, {0.741f, 0xac}, {0.757f, 0xae}, {0.773f, 0xb0}, {0.79f, 0xb2},
    {0.808f, 0xb4}, {0.825f, 0xb6}, {0.843f, 0xb8}, {0.862f, 0xba}, {0.881f, 0xbc},
    {0.9f, 0xbe}, {0.92f, 0xc0}, {0.94f, 0xc2}, {0.96f, 0xc4}, {0.981f, 0xc6},
    {1.003f, 0xc8}
};

static const struct { float amp; uint16_t code; } jc_lf_amp[101] = {
    {0.0f, 0x0040}, {0.01f, 0x8040}, {0.012f, 0x0041}, {0.014f, 0x8041}, {0.017f, 0x0042},
    {0.02f, 0x8042}, {0.024f, 0x0043}, {0.028f, 0x8043}, {0.033f, 0x0044}, {0.04f, 0x8044},
    {0.047f, 0x0045}, {0.056f, 0x8045}, {0.067f, 0x0046}, {0.08f, 0x8046}, {0.095f, 0x0047},
    {0.112f, 0x8047}, {0.117f, 0x0048}, {0.123f, 0x8048}, {0.128f, 0x0049}, {0.134f, 0x8049},
    {0.14f, 0x004a}, {0.146f, 0x804a}, {0.152f, 0x004b}, {0.159f, 0x804b}, {0.166f, 0x004c},
    {0.173f, 0x804c}, {0.181f, 0x004d}, {0.189f, 0x804d}, {0.198f, 0x004e}, {0.206f, 0x804e},
    {0.215f, 0x004f}, {0.225f, 0x804f}, {0.23f, 0x0050}, {0.235f, 0x8050}, {0.24f, 0x0051},
    {0.245f, 0x8051}, {0.251f, 0x0052}, {0.256f, 0x8052}, {0.262f, 0x0053}, {0.268f, 0x8053},
    {0.273f, 0x0054}, {0.279f, 0x8054}, {0.286f, 0x0055}, {0.292f, 0x8055}, {0.298f, 0x0056},
    {0.305f, 0x8056}, {0.311f, 0x0057}, {0.318f, 0x8057}, {0.325f, 0x0058}, {0.332f, 0x8058},
    {0.34f, 0x0059}, {0.347f, 0x8059}, {0.355f, 0x005a}, {0.362f, 0x805a}, {0.37f, 0x005b},
    {0.378f, 0x805b}, {0.387f, 0x005c}, {0.395f, 0x805c}, {0.404f, 0x005d}, {0.413f, 0x805d},
    {0.422f, 0x005e}, {0.431f, 0x805e}, {0.44f, 0x005f}, {0.45f, 0x805f}, {0.46f, 0x0060},
    {0.47f, 0x8060}, {0.48f, 0x0061}, {0.491f, 0x8061}, {0.501f, 0x0062}, {0.512f, 0x8062},
    {0.524f, 0x0063}, {0.535f, 0x8063}, {0.547f, 0x0064}, {0.559f, 0x8064}, {0.571f, 0x0065},
    {0.584f, 0x8065}, {0.596f, 0x0066}, {0.609f, 0x8066}, {0.623f, 0x0067}, {0.636f, 0x8067},
    {0.65f, 0x0068}, {0.665f, 0x8068}, {0.679f, 0x0069}, {0.694f, 0x8069}, {0.709f, 0x006a},
    {0.725f, 0x806a}, {0.741f, 0x006b}, {0.757f, 0x806b}, {0.773f, 0x006c}, {0.79f, 0x806c},
    {0.808f, 0x006d}, {0.825f, 0x806d}, {0.843f, 0x006e}, {0.862f, 0x806e}, {0.881f, 0x006f},
    {0.9f, 0x806f}, {0.92f, 0x0070}, {0.94f, 0x8070}, {0.96f, 0x0071}, {0.981f, 0x8071},
    {1.003f, 0x0072}
};

static inline uint8_t jc_encode_hf_amp(float amp) {
    for (int i = 0; i < 101; i++) {
        if (amp <= jc_hf_amp[i].amp) return jc_hf_amp[i].code;
    }
    return jc_hf_amp[100].code;
}

static inline uint16_t jc_encode_lf_amp(float amp) {
    for (int i = 0; i < 101; i++) {
        if (amp <= jc_lf_amp[i].amp) return jc_lf_amp[i].code;
    }
    return jc_lf_amp[100].code;
}

static void joycon_encode_vibration(const dm_vibration *v, uint8_t out[4]) {
    if (!v || (v->amplitude_high <= 0.0f && v->amplitude_low <= 0.0f)) {
        out[0] = 0x00; out[1] = 0x01; out[2] = 0x40; out[3] = 0x40;
        return;
    }
    float h_amp = v->amplitude_high;
    float l_amp = v->amplitude_low;
    float h_freq = v->frequency_high > 0 ? v->frequency_high : 320.0f;
    float l_freq = v->frequency_low > 0 ? v->frequency_low : 160.0f;

    float clamp_amp = 1.0f / fmaxf(1.0f, h_amp + l_amp);
    uint16_t enc_hf = jc_encode_hf(h_freq);
    uint8_t enc_hf_amp = jc_encode_hf_amp(h_amp * clamp_amp);
    uint8_t enc_lf = jc_encode_lf(l_freq);
    uint16_t enc_lf_amp = jc_encode_lf_amp(l_amp * clamp_amp);

    out[0] = (uint8_t)(enc_hf & 0xFF);
    out[1] = (uint8_t)(enc_hf_amp | ((enc_hf >> 8) & 0x01));
    out[2] = (uint8_t)(enc_lf | ((enc_lf_amp >> 8) & 0x80));
    out[3] = (uint8_t)(enc_lf_amp & 0xFF);
}

static void joycon_send_rumble(joycon_device *d, const dm_vibration *v) {
    if (!d || !d->hid) return;
    uint8_t enc[4];
    joycon_encode_vibration(v, enc);

    int is_neutral = (enc[0] == 0x00 && enc[1] == 0x01 && enc[2] == 0x40 && enc[3] == 0x40);
    if (is_neutral && !d->was_rumbling) return;

    uint8_t pkt[10];
    pkt[0] = 0x10; // Report 0x10: Rumble Only
    d->rumble_counter = (d->rumble_counter + 1) & 0x0F;
    pkt[1] = d->rumble_counter;
    memcpy(pkt + 2, enc, 4);
    memcpy(pkt + 6, enc, 4);

    IOReturn ret = IOHIDDeviceSetReport(d->hid, kIOHIDReportTypeOutput, 0x10, pkt, sizeof(pkt));
    if (ret == kIOReturnSuccess) {
        d->was_rumbling = !is_neutral;
    }
}

void dm_joycon_apply_haptics(dm_haptic_state s) {
    if (s.player != 0) return;
    pthread_mutex_lock(&g_joycon_dev_lock);
    if (g_jc_left_dev && g_jc_right_dev) {
        joycon_send_rumble(g_jc_left_dev, &s.motor[0]);
        joycon_send_rumble(g_jc_right_dev, &s.motor[1]);
    } else if (g_jc_left_dev) {
        const dm_vibration *v = (s.motor[0].amplitude_high > 0 || s.motor[0].amplitude_low > 0) ? &s.motor[0] : &s.motor[1];
        joycon_send_rumble(g_jc_left_dev, v);
    } else if (g_jc_right_dev) {
        const dm_vibration *v = (s.motor[1].amplitude_high > 0 || s.motor[1].amplitude_low > 0) ? &s.motor[1] : &s.motor[0];
        joycon_send_rumble(g_jc_right_dev, v);
    }
    pthread_mutex_unlock(&g_joycon_dev_lock);
}

static void joycon_send_subcmd(IOHIDDeviceRef dev, uint8_t subcmd, const uint8_t *data, size_t len) {
    uint8_t buf[49] = {0};
    buf[0] = 0x01; // Output report 0x01
    static uint8_t counter = 0;
    buf[1] = (counter++) & 0x0F;
    // Neutral rumble
    buf[2] = 0x00; buf[3] = 0x01; buf[4] = 0x40; buf[5] = 0x40;
    buf[6] = 0x00; buf[7] = 0x01; buf[8] = 0x40; buf[9] = 0x40;
    buf[10] = subcmd;
    if (data && len > 0) {
        memcpy(buf + 11, data, len > 38 ? 38 : len);
    }
    IOHIDDeviceSetReport(dev, kIOHIDReportTypeOutput, 0x01, buf, sizeof(buf));
}

static void joycon_publish_pad(void) {
    uint64_t btn = atomic_load(&g_jc_left_buttons) | atomic_load(&g_jc_right_buttons);
    int32_t lx = atomic_load(&g_jc_left_lx), ly = atomic_load(&g_jc_left_ly);
    int32_t rx = atomic_load(&g_jc_right_rx), ry = atomic_load(&g_jc_right_ry);
    dm_pad_set_player(0, btn, lx, ly, rx, ry);
}

static void joycon_report(void *ctx, IOReturn result, void *sender, IOHIDReportType type, uint32_t report_id, uint8_t *r, CFIndex len) {
    (void)sender;
    joycon_device *d = ctx;
    if (result != kIOReturnSuccess || type != kIOHIDReportTypeInput || len < 49 || report_id != 0x30) return;

    const float g = 1.0f / 4096.0f;
    const float rad = 0.070f * (float)M_PI / 180.0f;
    uint64_t now = now_ns();

    if (d->is_left) {
        // Buttons
        uint64_t b = 0;
        if (r[5] & 0x01) b |= DM_BTN_DOWN;
        if (r[5] & 0x02) b |= DM_BTN_UP;
        if (r[5] & 0x04) b |= DM_BTN_RIGHT;
        if (r[5] & 0x08) b |= DM_BTN_LEFT;
        if (r[5] & 0x40) b |= DM_BTN_L;
        if (r[5] & 0x80) b |= DM_BTN_ZL;
        if (r[4] & 0x01) b |= DM_BTN_MINUS;
        if (r[4] & 0x08) b |= DM_BTN_LS;
        atomic_store(&g_jc_left_buttons, b);

        // Stick
        uint16_t raw_lx = (uint16_t)(r[6] | ((r[7] & 0x0f) << 8));
        uint16_t raw_ly = (uint16_t)((r[7] >> 4) | (r[8] << 4));
        int32_t dx = (int32_t)raw_lx - 2048;
        int32_t dy = (int32_t)raw_ly - 2048;
        if (abs(dx) < 150) dx = 0;
        if (abs(dy) < 150) dy = 0;
        atomic_store(&g_jc_left_lx, (int32_t)fminf(fmaxf((float)dx * 32767.f / 1600.f, -32767.f), 32767.f));
        atomic_store(&g_jc_left_ly, (int32_t)fminf(fmaxf((float)dy * 32767.f / 1600.f, -32767.f), 32767.f));

        // 3 IMU frames (offset 13 + 12 * k)
        for (int k = 0; k < 3; k++) {
            const uint8_t *b_imu = r + 13 + 12 * k;
            int16_t v[6];
            for (int i = 0; i < 6; i++) {
                v[i] = (int16_t)((uint16_t)b_imu[2 * i] | ((uint16_t)b_imu[2 * i + 1] << 8));
            }
            float acc[3] = { v[1] * g, -v[0] * g, -v[2] * g };
            float rot[3] = { -v[4] * rad, v[3] * rad, v[5] * rad };
            uint64_t ns = now - (uint64_t)(2 - k) * 5000000;
            dm_motion_set_hid_device(0, 0, acc, rot, ns);
        }
    } else {
        // Right Joy-Con
        uint64_t b = 0;
        if (r[3] & 0x01) b |= DM_BTN_Y;
        if (r[3] & 0x02) b |= DM_BTN_X;
        if (r[3] & 0x04) b |= DM_BTN_B;
        if (r[3] & 0x08) b |= DM_BTN_A;
        if (r[3] & 0x40) b |= DM_BTN_R;
        if (r[3] & 0x80) b |= DM_BTN_ZR;
        if (r[4] & 0x02) b |= DM_BTN_PLUS;
        if (r[4] & 0x04) b |= DM_BTN_RS;
        atomic_store(&g_jc_right_buttons, b);

        // Stick
        uint16_t raw_rx = (uint16_t)(r[9] | ((r[10] & 0x0f) << 8));
        uint16_t raw_ry = (uint16_t)((r[10] >> 4) | (r[11] << 4));
        int32_t dx = (int32_t)raw_rx - 2048;
        int32_t dy = (int32_t)raw_ry - 2048;
        if (abs(dx) < 150) dx = 0;
        if (abs(dy) < 150) dy = 0;
        atomic_store(&g_jc_right_rx, (int32_t)fminf(fmaxf((float)dx * 32767.f / 1600.f, -32767.f), 32767.f));
        atomic_store(&g_jc_right_ry, (int32_t)fminf(fmaxf((float)dy * 32767.f / 1600.f, -32767.f), 32767.f));

        // 3 IMU frames
        for (int k = 0; k < 3; k++) {
            const uint8_t *b_imu = r + 13 + 12 * k;
            int16_t v[6];
            for (int i = 0; i < 6; i++) {
                v[i] = (int16_t)((uint16_t)b_imu[2 * i] | ((uint16_t)b_imu[2 * i + 1] << 8));
            }
            float acc[3] = { -v[1] * g, -v[0] * g, v[2] * g };
            float rot[3] = { v[4] * rad, v[3] * rad, -v[5] * rad };
            uint64_t ns = now - (uint64_t)(2 - k) * 5000000;
            dm_motion_set_hid_device(0, 1, acc, rot, ns);
        }
    }
    joycon_publish_pad();
}

static void joycon_found(void *ctx, IOReturn result, void *sender, IOHIDDeviceRef hid) {
    (void)ctx; (void)sender;
    if (result != kIOReturnSuccess || IOHIDDeviceGetProperty(hid, CFSTR(kIOHIDGCSyntheticDeviceKey)) == kCFBooleanTrue) return;
    for (joycon_device *d = g_joycon_devices; d; d = d->next) if (d->hid == hid) return;

    NSNumber *product = (__bridge NSNumber *)IOHIDDeviceGetProperty(hid, CFSTR(kIOHIDProductIDKey));
    int pid = product ? product.intValue : 0;
    int is_left = (pid == 0x2006);

    joycon_device *d = calloc(1, sizeof *d);
    if (!d) return;
    d->hid = (IOHIDDeviceRef)CFRetain(hid);
    d->is_left = is_left;
    d->next = g_joycon_devices;
    g_joycon_devices = d;

    if (is_left) {
        atomic_store(&g_joycon_left_connected, 1);
    } else {
        atomic_store(&g_joycon_right_connected, 1);
    }

    pthread_mutex_lock(&g_joycon_dev_lock);
    if (is_left) g_jc_left_dev = d;
    else g_jc_right_dev = d;
    pthread_mutex_unlock(&g_joycon_dev_lock);

    LOG("[hid] Joy-Con (%s) connected (PID 0x%04x)", is_left ? "L" : "R", pid);

    // 1. Enable 6-axis IMU (subcmd 0x40, arg 0x01)
    uint8_t imu_on = 0x01;
    joycon_send_subcmd(hid, 0x40, &imu_on, 1);

    // 2. Set report mode to 0x30 standard full report (subcmd 0x03, arg 0x30)
    uint8_t mode_30 = 0x30;
    joycon_send_subcmd(hid, 0x03, &mode_30, 1);

    // 3. Set player 1 LED pattern solid (subcmd 0x30, arg 0x01)
    uint8_t led_p1 = 0x01;
    joycon_send_subcmd(hid, 0x30, &led_p1, 1);

    // 4. Enable vibration (subcmd 0x48, arg 0x01)
    uint8_t vib_on = 0x01;
    joycon_send_subcmd(hid, 0x48, &vib_on, 1);

    dm_haptics_route(0, 1);

    IOHIDDeviceRegisterInputReportCallback(hid, d->report, sizeof d->report, joycon_report, d);
}

static void joycon_removed(void *ctx, IOReturn result, void *sender, IOHIDDeviceRef hid) {
    (void)ctx; (void)result; (void)sender;
    joycon_device **link = &g_joycon_devices;
    while (*link && (*link)->hid != hid) link = &(*link)->next;
    if (!*link) return;
    joycon_device *d = *link;
    *link = d->next;

    IOHIDDeviceRegisterInputReportCallback(hid, d->report, sizeof d->report, NULL, NULL);

    if (d->was_rumbling) {
        uint8_t pkt[10] = {0x10, (uint8_t)((d->rumble_counter++) & 0x0F), 0x00, 0x01, 0x40, 0x40, 0x00, 0x01, 0x40, 0x40};
        IOHIDDeviceSetReport(hid, kIOHIDReportTypeOutput, 0x10, pkt, sizeof(pkt));
    }

    pthread_mutex_lock(&g_joycon_dev_lock);
    if (d->is_left && g_jc_left_dev == d) g_jc_left_dev = NULL;
    if (!d->is_left && g_jc_right_dev == d) g_jc_right_dev = NULL;
    pthread_mutex_unlock(&g_joycon_dev_lock);

    if (d->is_left) {
        atomic_store(&g_joycon_left_connected, 0);
        atomic_store(&g_jc_left_buttons, 0);
        atomic_store(&g_jc_left_lx, 0);
        atomic_store(&g_jc_left_ly, 0);
        dm_motion_clear_hid_device(0, 0);
        LOG("[hid] Joy-Con (L) disconnected");
    } else {
        atomic_store(&g_joycon_right_connected, 0);
        atomic_store(&g_jc_right_buttons, 0);
        atomic_store(&g_jc_right_rx, 0);
        atomic_store(&g_jc_right_ry, 0);
        dm_motion_clear_hid_device(0, 1);
        LOG("[hid] Joy-Con (R) disconnected");
    }
    joycon_publish_pad();

    if (dm_joycon_count() == 0) {
        dm_haptics_route(0, 0);
    }

    CFRelease(d->hid);
    free(d);
}

static void dm_joycon_start(void) {
    const char *e = getenv("DM_JOYCON");
    if (e && !atoi(e)) return;

    g_joycon_manager = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
    if (!g_joycon_manager) return;

    NSArray *match = @[
        @{@kIOHIDVendorIDKey: @0x057E, @kIOHIDProductIDKey: @0x2006},
        @{@kIOHIDVendorIDKey: @0x057E, @kIOHIDProductIDKey: @0x2007}
    ];
    IOHIDManagerSetDeviceMatchingMultiple(g_joycon_manager, (__bridge CFArrayRef)match);
    IOHIDManagerRegisterDeviceMatchingCallback(g_joycon_manager, joycon_found, NULL);
    IOHIDManagerRegisterDeviceRemovalCallback(g_joycon_manager, joycon_removed, NULL);
    IOHIDManagerScheduleWithRunLoop(g_joycon_manager, CFRunLoopGetMain(), kCFRunLoopCommonModes);
    IOReturn r = IOHIDManagerOpen(g_joycon_manager, kIOHIDOptionsTypeNone);
    if (r != kIOReturnSuccess) {
        LOG("[RED] Joy-Con raw motion: cannot open HID (0x%x)", r);
        IOHIDManagerUnscheduleFromRunLoop(g_joycon_manager, CFRunLoopGetMain(), kCFRunLoopCommonModes);
        CFRelease(g_joycon_manager);
        g_joycon_manager = NULL;
    } else {
        LOG("[hid] Joy-Con driver started (monitoring Joy-Con L/R)");
    }
}
