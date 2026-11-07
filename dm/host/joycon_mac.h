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
    int stop_sends;
    uint8_t last_rumble_enc[4];
    pthread_mutex_t hid_write_lock;
    pthread_mutex_t rumble_lock;
    pthread_cond_t rumble_cv;
    dm_vibration rumble_target;
    float rumble_acc_lo;
    float rumble_acc_hi;
    int rumble_samples;
    int rumble_dirty;
    _Atomic int refs;
    _Atomic int removed;
    _Atomic int ring_connected;
    _Atomic int ring_probing;
    _Atomic int init_done;
    int16_t ring_center;
    int16_t ring_min;
    int16_t ring_max;
    int ring_log_bucket;
    int16_t acc_offset[3];
    int16_t acc_scale[3];
    int16_t gyr_offset[3];
    int16_t gyr_scale[3];
    float gyro_bias[3];
    int16_t stick_center[2];
    int16_t stick_min[2];
    int16_t stick_max[2];
    int16_t stick_deadzone;
    uint8_t last_pkt_id;
    int has_pkt_id;
    uint64_t imu_ns;
    pthread_mutex_t subcmd_lock;
    pthread_cond_t subcmd_cv;
    uint32_t subcmd_seq;
    uint8_t last_subcmd_ack;
    uint8_t last_subcmd_id;
    uint8_t last_subcmd_data[34];
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
static _Atomic int g_ringcon_wanted, g_jc_ring_connected;
static _Atomic uint32_t g_jc_ring_force_bits;
static _Atomic int g_jc_left_battery = -1;
static _Atomic int g_jc_left_charging = 0;
static _Atomic int g_jc_right_battery = -1;
static _Atomic int g_jc_right_charging = 0;

static void joycon_retain(joycon_device *d) {
    atomic_fetch_add(&d->refs, 1);
}

static void joycon_release(joycon_device *d) {
    if (atomic_fetch_sub(&d->refs, 1) == 1) {
        if (d->hid) CFRelease(d->hid);
        pthread_mutex_destroy(&d->hid_write_lock);
        pthread_mutex_destroy(&d->rumble_lock);
        pthread_cond_destroy(&d->rumble_cv);
        pthread_mutex_destroy(&d->subcmd_lock);
        pthread_cond_destroy(&d->subcmd_cv);
        free(d);
    }
}

int dm_ringcon_active(void) {
    return atomic_load(&g_jc_ring_connected);
}

float dm_ringcon_force(void) {
    uint32_t bits = atomic_load(&g_jc_ring_force_bits);
    float f;
    memcpy(&f, &bits, sizeof(f));
    return f;
}

int dm_joycon_is_dual(void) {
    return atomic_load(&g_joycon_left_connected) && atomic_load(&g_joycon_right_connected);
}

int dm_joycon_count(void) {
    return (atomic_load(&g_joycon_left_connected) ? 1 : 0) + (atomic_load(&g_joycon_right_connected) ? 1 : 0);
}

int dm_joycon_has_haptics(void) {
    return atomic_load(&g_joycon_left_connected) || atomic_load(&g_joycon_right_connected);
}

int dm_joycon_get_battery(int is_left, int *charging) {
    if (is_left) {
        if (!atomic_load(&g_joycon_left_connected)) return -1;
        if (charging) *charging = atomic_load(&g_jc_left_charging);
        return atomic_load(&g_jc_left_battery);
    } else {
        if (!atomic_load(&g_joycon_right_connected)) return -1;
        if (charging) *charging = atomic_load(&g_jc_right_charging);
        return atomic_load(&g_jc_right_battery);
    }
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

static void *joycon_rumble_thread(void *arg) {
    joycon_device *d = arg;
    while (!atomic_load(&d->removed)) {
        pthread_mutex_lock(&d->rumble_lock);
        while (!d->rumble_dirty && !d->was_rumbling && d->stop_sends >= 2 && !atomic_load(&d->removed)) {
            pthread_cond_wait(&d->rumble_cv, &d->rumble_lock);
        }
        if (atomic_load(&d->removed)) {
            pthread_mutex_unlock(&d->rumble_lock);
            break;
        }
        dm_vibration v = d->rumble_target;
        if (d->rumble_samples > 0) {
            // Average amplitude over the 35 ms Bluetooth transmission window so brief 5 ms
            // LRA impulses do not get held at peak amplitude for 35 ms.
            float avg_lo = d->rumble_acc_lo / (float)d->rumble_samples;
            float avg_hi = d->rumble_acc_hi / (float)d->rumble_samples;
            if (v.amplitude_low > 0.0f || v.amplitude_high > 0.0f) {
                v.amplitude_low  = fminf(avg_lo * 0.35f, 0.40f);
                v.amplitude_high = fminf(avg_hi * 0.35f, 0.40f);
            }
        }
        d->rumble_acc_lo = 0.0f;
        d->rumble_acc_hi = 0.0f;
        d->rumble_samples = 0;
        d->rumble_dirty = 0;
        pthread_mutex_unlock(&d->rumble_lock);

        if (!atomic_load(&d->ring_probing) && d->hid) {
            uint8_t enc[4];
            joycon_encode_vibration(&v, enc);
            int is_neutral = (enc[0] == 0x00 && enc[1] == 0x01 && enc[2] == 0x40 && enc[3] == 0x40);

            int should_send = 0;
            if (is_neutral) {
                if (d->was_rumbling || d->stop_sends < 2) should_send = 1;
            } else {
                if (!d->was_rumbling || memcmp(enc, d->last_rumble_enc, 4) != 0) should_send = 1;
            }

            if (should_send) {
                uint8_t pkt[10];
                pkt[0] = 0x10; // Report 0x10: Rumble Only
                d->rumble_counter = (d->rumble_counter + 1) & 0x0F;
                pkt[1] = d->rumble_counter;
                memcpy(pkt + 2, enc, 4);
                memcpy(pkt + 6, enc, 4);

                pthread_mutex_lock(&d->hid_write_lock);
                IOReturn ret = IOHIDDeviceSetReport(d->hid, kIOHIDReportTypeOutput, 0x10, pkt, sizeof(pkt));
                pthread_mutex_unlock(&d->hid_write_lock);

                if (ret == kIOReturnSuccess) {
                    memcpy(d->last_rumble_enc, enc, 4);
                    if (is_neutral) {
                        d->was_rumbling = 0;
                        d->stop_sends++;
                    } else {
                        d->was_rumbling = 1;
                        d->stop_sends = 0;
                    }
                }
            }
        }

        // Pace Bluetooth HID Control Channel SET_REPORT transactions at ~28 Hz (35 ms)
        // so IOBluetoothHIDDriver never collides and enters its 5-10s transaction timeout.
        usleep(35000);
    }
    joycon_release(d);
    return NULL;
}

static void joycon_queue_rumble(joycon_device *d, const dm_vibration *v) {
    if (!d || atomic_load(&d->removed)) return;
    pthread_mutex_lock(&d->rumble_lock);
    d->rumble_target = *v;
    d->rumble_acc_lo += v->amplitude_low;
    d->rumble_acc_hi += v->amplitude_high;
    d->rumble_samples++;
    d->rumble_dirty = 1;
    pthread_cond_signal(&d->rumble_cv);
    pthread_mutex_unlock(&d->rumble_lock);
}

void dm_joycon_apply_haptics(dm_haptic_state s) {
    if (s.player != 0) return;
    pthread_mutex_lock(&g_joycon_dev_lock);
    if (g_jc_left_dev && g_jc_right_dev) {
        joycon_queue_rumble(g_jc_left_dev, &s.motor[0]);
        joycon_queue_rumble(g_jc_right_dev, &s.motor[1]);
    } else if (g_jc_left_dev) {
        const dm_vibration *v = (s.motor[0].amplitude_high > 0 || s.motor[0].amplitude_low > 0) ? &s.motor[0] : &s.motor[1];
        joycon_queue_rumble(g_jc_left_dev, v);
    } else if (g_jc_right_dev) {
        const dm_vibration *v = (s.motor[1].amplitude_high > 0 || s.motor[1].amplitude_low > 0) ? &s.motor[1] : &s.motor[0];
        joycon_queue_rumble(g_jc_right_dev, v);
    }
    pthread_mutex_unlock(&g_joycon_dev_lock);
}

static void joycon_send_subcmd(joycon_device *d, uint8_t subcmd, const uint8_t *data, size_t len) {
    if (!d || !d->hid) return;
    uint8_t buf[49] = {0};
    buf[0] = 0x01; // Output report 0x01
    static _Atomic uint8_t counter = 0;
    buf[1] = atomic_fetch_add(&counter, 1) & 0x0F;
    // Neutral rumble
    buf[2] = 0x00; buf[3] = 0x01; buf[4] = 0x40; buf[5] = 0x40;
    buf[6] = 0x00; buf[7] = 0x01; buf[8] = 0x40; buf[9] = 0x40;
    buf[10] = subcmd;
    if (data && len > 0) {
        memcpy(buf + 11, data, len > 38 ? 38 : len);
    }
    pthread_mutex_lock(&d->hid_write_lock);
    IOHIDDeviceSetReport(d->hid, kIOHIDReportTypeOutput, 0x01, buf, sizeof(buf));
    pthread_mutex_unlock(&d->hid_write_lock);
}

// CRC-8-CCITT (polynomial 0x07) used by Joy-Con MCU configuration packets (Subcommand 0x21)
static uint8_t jc_mcu_crc8(const uint8_t *buf, size_t len) {
    uint8_t crc = 0;
    for (size_t i = 0; i < len; i++) {
        crc ^= buf[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

static int joycon_send_subcmd_sync(joycon_device *d, uint8_t subcmd, const uint8_t *data, size_t len, uint8_t reply_out[34], int max_tries) {
    for (int t = 0; t < max_tries && !atomic_load(&d->removed); t++) {
        pthread_mutex_lock(&d->subcmd_lock);
        uint32_t seq = d->subcmd_seq;
        pthread_mutex_unlock(&d->subcmd_lock);

        joycon_send_subcmd(d, subcmd, data, len);

        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 66000000; // 66 ms timeout per attempt (matching Eden's GetSubCommandResponse)
        if (ts.tv_nsec >= 1000000000) { ts.tv_sec += 1; ts.tv_nsec -= 1000000000; }

        pthread_mutex_lock(&d->subcmd_lock);
        while (!atomic_load(&d->removed) && (d->subcmd_seq == seq || d->last_subcmd_id != subcmd)) {
            if (pthread_cond_timedwait(&d->subcmd_cv, &d->subcmd_lock, &ts) == ETIMEDOUT) break;
        }
        int ok = !atomic_load(&d->removed) && d->subcmd_seq != seq && d->last_subcmd_id == subcmd;
        if (ok && reply_out) memcpy(reply_out, d->last_subcmd_data, 34);
        pthread_mutex_unlock(&d->subcmd_lock);
        if (ok) return 1;
    }
    return 0;
}

static int joycon_read_spi(joycon_device *d, uint16_t addr, uint8_t size, uint8_t *out) {
    if (size > 29) return 0;
    uint8_t req[5] = {(uint8_t)(addr & 0xFF), (uint8_t)((addr >> 8) & 0xFF), 0x00, 0x00, size};
    for (int tries = 0; tries < 5 && !atomic_load(&d->removed); tries++) {
        uint8_t reply[34] = {0};
        if (joycon_send_subcmd_sync(d, 0x10, req, sizeof(req), reply, 4)) {
            uint16_t r_addr = (uint16_t)reply[0] | ((uint16_t)reply[1] << 8);
            if (r_addr == addr && reply[4] == size) {
                memcpy(out, reply + 5, size);
                return 1;
            }
        }
    }
    return 0;
}

static inline int16_t jc_validate_cal(int16_t val, int16_t def) {
    if (val == 0 || val == 0x0FFF || val == -1) return def;
    return val;
}

static void joycon_load_imu_calibration(joycon_device *d) {
    uint8_t magic[2] = {0};
    uint8_t cal[24] = {0};
    int is_user = 0;
    if (joycon_read_spi(d, 0x8026, 2, magic) && magic[0] == 0xB2 && magic[1] == 0xA1) {
        if (joycon_read_spi(d, 0x8028, 24, cal)) is_user = 1;
    }
    if (!is_user && !joycon_read_spi(d, 0x6020, 24, cal)) return;

    int16_t v[12];
    for (int i = 0; i < 12; i++) {
        v[i] = (int16_t)((uint16_t)cal[2 * i] | ((uint16_t)cal[2 * i + 1] << 8));
    }
    for (int i = 0; i < 3; i++) {
        int16_t ao = jc_validate_cal(v[0 + i], 0);
        int16_t as = jc_validate_cal(v[3 + i], 0x4000);
        int16_t go = jc_validate_cal(v[6 + i], 0);
        int16_t gs = jc_validate_cal(v[9 + i], 0x3be7);
        if ((int32_t)as - (int32_t)ao > 2000) {
            d->acc_offset[i] = ao;
            d->acc_scale[i] = as;
        }
        if ((int32_t)gs - (int32_t)go > 2000) {
            d->gyr_offset[i] = go;
            d->gyr_scale[i] = gs;
        }
    }
    LOG("[hid] Joy-Con (%s) IMU %s calibration: acc_off=(%d,%d,%d) acc_scale=(%d,%d,%d) gyr_off=(%d,%d,%d) gyr_scale=(%d,%d,%d)",
        d->is_left ? "L" : "R", is_user ? "user" : "factory",
        d->acc_offset[0], d->acc_offset[1], d->acc_offset[2],
        d->acc_scale[0], d->acc_scale[1], d->acc_scale[2],
        d->gyr_offset[0], d->gyr_offset[1], d->gyr_offset[2],
        d->gyr_scale[0], d->gyr_scale[1], d->gyr_scale[2]);
}

static void joycon_load_stick_calibration(joycon_device *d) {
    uint8_t magic[2] = {0};
    uint8_t cal[9] = {0};
    int is_user = 0;
    uint16_t user_magic_addr = d->is_left ? 0x8010 : 0x801B;
    uint16_t user_data_addr  = d->is_left ? 0x8012 : 0x801D;
    uint16_t factory_addr    = d->is_left ? 0x603D : 0x6046;
    uint16_t param_addr      = d->is_left ? 0x6086 : 0x6098;

    if (joycon_read_spi(d, user_magic_addr, 2, magic) && magic[0] == 0xB2 && magic[1] == 0xA1) {
        if (joycon_read_spi(d, user_data_addr, 9, cal)) is_user = 1;
    }
    if (!is_user && !joycon_read_spi(d, factory_addr, 9, cal)) return;

    int16_t center_x, center_y, max_above_x, max_above_y, min_below_x, min_below_y;
    if (d->is_left) {
        max_above_x = (int16_t)(cal[0] | ((cal[1] & 0x0F) << 8));
        max_above_y = (int16_t)((cal[1] >> 4) | (cal[2] << 4));
        center_x    = (int16_t)(cal[3] | ((cal[4] & 0x0F) << 8));
        center_y    = (int16_t)((cal[4] >> 4) | (cal[5] << 4));
        min_below_x = (int16_t)(cal[6] | ((cal[7] & 0x0F) << 8));
        min_below_y = (int16_t)((cal[7] >> 4) | (cal[8] << 4));
    } else {
        center_x    = (int16_t)(cal[0] | ((cal[1] & 0x0F) << 8));
        center_y    = (int16_t)((cal[1] >> 4) | (cal[2] << 4));
        min_below_x = (int16_t)(cal[3] | ((cal[4] & 0x0F) << 8));
        min_below_y = (int16_t)((cal[4] >> 4) | (cal[5] << 4));
        max_above_x = (int16_t)(cal[6] | ((cal[7] & 0x0F) << 8));
        max_above_y = (int16_t)((cal[7] >> 4) | (cal[8] << 4));
    }

    if (center_x >= 500 && center_x <= 3500 && center_y >= 500 && center_y <= 3500 &&
        max_above_x >= 300 && min_below_x >= 300 && max_above_y >= 300 && min_below_y >= 300) {
        d->stick_center[0] = center_x;
        d->stick_center[1] = center_y;
        d->stick_max[0] = center_x + max_above_x;
        d->stick_min[0] = center_x - min_below_x;
        d->stick_max[1] = center_y + max_above_y;
        d->stick_min[1] = center_y - min_below_y;
    }

    uint8_t params[9] = {0};
    if (joycon_read_spi(d, param_addr, 9, params)) {
        uint16_t dz = (uint16_t)(params[3] | ((params[4] & 0x0F) << 8));
        if (dz >= 80 && dz <= 400) {
            d->stick_deadzone = (int16_t)dz;
        }
    }

    LOG("[hid] Joy-Con (%s) stick %s calibration: center=(%d,%d) min=(%d,%d) max=(%d,%d) deadzone=%d",
        d->is_left ? "L" : "R", is_user ? "user" : "factory",
        d->stick_center[0], d->stick_center[1],
        d->stick_min[0], d->stick_min[1],
        d->stick_max[0], d->stick_max[1],
        d->stick_deadzone);
}

static void joycon_process_stick(joycon_device *d, uint16_t raw_x, uint16_t raw_y, int32_t *out_x, int32_t *out_y) {
    int32_t cx = d->stick_center[0] ? d->stick_center[0] : 2048;
    int32_t cy = d->stick_center[1] ? d->stick_center[1] : 2048;
    int32_t dx = (int32_t)raw_x - cx;
    int32_t dy = (int32_t)raw_y - cy;
    int32_t dz = d->stick_deadzone > 0 ? d->stick_deadzone : 200;
    const char *env_dz = getenv("DM_JOYCON_DEADZONE");
    if (env_dz && atoi(env_dz) > 0) dz = atoi(env_dz);
    else if (dz < 220) dz = 220;

    int64_t dist_sq = (int64_t)dx * dx + (int64_t)dy * dy;
    if (dist_sq <= (int64_t)dz * dz) {
        *out_x = 0;
        *out_y = 0;
        return;
    }

    float dist = sqrtf((float)dist_sq);
    float factor = (dist - (float)dz) / dist;
    float fdx = (float)dx * factor;
    float fdy = (float)dy * factor;

    int32_t max_x = (d->stick_max[0] > cx + dz) ? (d->stick_max[0] - cx - dz) : (1600 - dz);
    int32_t min_x = (cx - dz > d->stick_min[0]) ? (cx - dz - d->stick_min[0]) : (1600 - dz);
    int32_t max_y = (d->stick_max[1] > cy + dz) ? (d->stick_max[1] - cy - dz) : (1600 - dz);
    int32_t min_y = (cy - dz > d->stick_min[1]) ? (cy - dz - d->stick_min[1]) : (1600 - dz);
    if (max_x <= 0) max_x = 1;
    if (min_x <= 0) min_x = 1;
    if (max_y <= 0) max_y = 1;
    if (min_y <= 0) min_y = 1;

    float sx = (fdx > 0.0f) ? (fdx * 32767.0f / (float)max_x) : (fdx * 32767.0f / (float)min_x);
    float sy = (fdy > 0.0f) ? (fdy * 32767.0f / (float)max_y) : (fdy * 32767.0f / (float)min_y);

    *out_x = (int32_t)fminf(fmaxf(sx, -32767.0f), 32767.0f);
    *out_y = (int32_t)fminf(fmaxf(sy, -32767.0f), 32767.0f);
}

static void joycon_maybe_start_ringcon(joycon_device *d);

static void *joycon_init_thread(void *arg) {
    joycon_device *d = arg;
    atomic_store(&d->ring_probing, 1);

    // 1. Read SPI flash IMU and stick calibrations
    joycon_load_imu_calibration(d);
    joycon_load_stick_calibration(d);

    // 2. Enable 6-axis IMU (ENABLE_IMU = 0x40, arg = 0x01)
    uint8_t imu_on = 0x01;
    joycon_send_subcmd_sync(d, 0x40, &imu_on, 1, NULL, 4);

    // 3. Set IMU sensitivity to DPS2000, G8, HZ833, HZ100 (SET_IMU_SENSITIVITY = 0x41)
    static const uint8_t imu_cfg[4] = {0x03, 0x00, 0x00, 0x01};
    joycon_send_subcmd_sync(d, 0x41, imu_cfg, sizeof(imu_cfg), NULL, 4);

    // 4. Enable vibration (ENABLE_VIBRATION = 0x48, arg = 0x01)
    uint8_t vib_on = 0x01;
    joycon_send_subcmd_sync(d, 0x48, &vib_on, 1, NULL, 4);

    // 5. Set player 1 LED pattern solid (SET_PLAYER_LIGHTS = 0x30, arg = 0x01)
    uint8_t led_p1 = 0x01;
    joycon_send_subcmd_sync(d, 0x30, &led_p1, 1, NULL, 4);

    // 6. Set report mode to 0x30 standard full 60Hz report (SET_REPORT_MODE = 0x03)
    uint8_t mode_30 = 0x30;
    joycon_send_subcmd_sync(d, 0x03, &mode_30, 1, NULL, 6);

    // Reset motion state so it primes cleanly with the calibrated IMU stream
    d->imu_ns = 0;
    dm_motion_clear_hid_device(0, d->is_left ? 0 : 1);

    atomic_store(&d->init_done, 1);
    atomic_store(&d->ring_probing, 0);

    if (!d->is_left && atomic_load(&g_ringcon_wanted)) {
        joycon_maybe_start_ringcon(d);
    }

    joycon_release(d);
    return NULL;
}

static void *ringcon_init_thread(void *arg) {
    joycon_device *d = arg;
    while (!atomic_load(&d->removed) && atomic_load(&g_ringcon_wanted) && !atomic_load(&d->ring_connected)) {
        atomic_store(&d->ring_probing, 1);

        // 1. Ensure Report Mode 0x30 (SET_REPORT_MODE = 0x03)
        uint8_t mode_30 = 0x30;
        joycon_send_subcmd_sync(d, 0x03, &mode_30, 1, NULL, 6);

        // 2. Enable MCU (SET_MCU_STATE = 0x22, arg = 0x01)
        uint8_t mcu_on = 0x01;
        joycon_send_subcmd_sync(d, 0x22, &mcu_on, 1, NULL, 6);

        // 3. Configure MCU device mode to Standby (SET_MCU_CONFIG = 0x21)
        uint8_t mcu_cfg[38] = {0};
        mcu_cfg[0] = 0x21; // ConfigureMCU
        mcu_cfg[1] = 0x01; // SetDeviceMode
        mcu_cfg[2] = 0x01; // Standby
        mcu_cfg[37] = jc_mcu_crc8(mcu_cfg + 1, 36);
        joycon_send_subcmd_sync(d, 0x21, mcu_cfg, sizeof(mcu_cfg), NULL, 6);

        // 4. Poll GET_EXTERNAL_DEVICE_INFO (0x59) for RingController (0x2000)
        int found_ring = 0;
        for (int tries = 0; tries < 42 && !atomic_load(&d->removed) && atomic_load(&g_ringcon_wanted); tries++) {
            uint8_t reply[34] = {0};
            if (joycon_send_subcmd_sync(d, 0x59, NULL, 0, reply, 3)) {
                uint16_t ext_id = (uint16_t)reply[0] | ((uint16_t)reply[1] << 8);
                if (ext_id == 0x2000) {
                    found_ring = 1;
                    break;
                }
            }
            usleep(40000);
        }

        if (found_ring && !atomic_load(&d->removed) && atomic_load(&g_ringcon_wanted)) {
            // 5. Configure Ring-Con external format (SET_EXTERNAL_CONFIG = 0x5C)
            static const uint8_t ring_config[37] = {
                0x06, 0x03, 0x25, 0x06, 0x00, 0x00, 0x00, 0x00, 0x1C, 0x16, 0xED, 0x34, 0x36,
                0x00, 0x00, 0x00, 0x0A, 0x64, 0x0B, 0xE6, 0xA9, 0x22, 0x00, 0x00, 0x04, 0x00,
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x90, 0xA8, 0xE1, 0x34, 0x36
            };
            joycon_send_subcmd_sync(d, 0x5C, ring_config, sizeof(ring_config), NULL, 6);

            // 6. Enable Ring-Con external polling (ENABLE_EXTERNAL_POLLING = 0x5A)
            static const uint8_t ext_poll[4] = {0x04, 0x01, 0x01, 0x02};
            if (joycon_send_subcmd_sync(d, 0x5A, ext_poll, sizeof(ext_poll), NULL, 6)) {
                usleep(50000);
                d->ring_center = 0;
                d->ring_min = 0;
                d->ring_max = 0;
                d->ring_log_bucket = 0;
                atomic_store(&d->ring_connected, 1);
                atomic_store(&g_jc_ring_connected, 1);
                atomic_store(&d->ring_probing, 0);
                LOG("[hid] Ring-Con detected on Joy-Con (R) and external polling enabled");
                break;
            }
        }

        atomic_store(&d->ring_probing, 0);
        for (int w = 0; w < 10 && !atomic_load(&d->removed) && atomic_load(&g_ringcon_wanted); w++) {
            usleep(100000);
        }
    }
    atomic_store(&d->ring_probing, 0);
    joycon_release(d);
    return NULL;
}

static void joycon_maybe_start_ringcon(joycon_device *d) {
    if (!d || d->is_left || atomic_load(&d->removed) || atomic_load(&d->ring_connected)) return;
    if (!atomic_load(&g_ringcon_wanted) || !atomic_load(&d->init_done)) return;
    if (atomic_exchange(&d->ring_probing, 1)) return;
    joycon_retain(d);
    pthread_t th;
    if (pthread_create(&th, NULL, ringcon_init_thread, d) == 0) {
        pthread_detach(th);
    } else {
        atomic_store(&d->ring_probing, 0);
        joycon_release(d);
    }
}

void dm_ringcon_enable(int enable) {
    int prev = atomic_exchange(&g_ringcon_wanted, enable ? 1 : 0);
    if (!enable) {
        atomic_store(&g_jc_ring_connected, 0);
        atomic_store(&g_jc_ring_force_bits, 0);
        return;
    }
    if (!prev) {
        LOG("[hid] Ring-Con polling requested by game (hidbus enabled)");
    }
    pthread_mutex_lock(&g_joycon_dev_lock);
    joycon_device *d = g_jc_right_dev;
    if (d) joycon_maybe_start_ringcon(d);
    pthread_mutex_unlock(&g_joycon_dev_lock);
}

static void joycon_publish_pad(void) {
    uint64_t btn = atomic_load(&g_jc_left_buttons) | atomic_load(&g_jc_right_buttons);
    int32_t lx = atomic_load(&g_jc_left_lx), ly = atomic_load(&g_jc_left_ly);
    int32_t rx = atomic_load(&g_jc_right_rx), ry = atomic_load(&g_jc_right_ry);
    dm_pad_set_player(0, btn, lx, ly, rx, ry);
}

static void joycon_decode_imu(joycon_device *d, const uint8_t *b_imu, float acc_out[3], float rot_out[3]) {
    int16_t v[6];
    for (int i = 0; i < 6; i++) {
        v[i] = (int16_t)((uint16_t)b_imu[2 * i] | ((uint16_t)b_imu[2 * i + 1] << 8));
    }
    // Match Eden's JoyconPoller::GetMotionInput (poller.cpp:313-372)
    float ax = (float)(v[1] - d->acc_offset[1]) * 4.0f / (float)(d->acc_scale[1] - d->acc_offset[1]);
    float ay = (float)(v[0] - d->acc_offset[0]) * 4.0f / (float)(d->acc_scale[0] - d->acc_offset[0]);
    float az = (float)(v[2] - d->acc_offset[2]) * 4.0f / (float)(d->acc_scale[2] - d->acc_offset[2]);

    const float deg_to_rad = (float)M_PI / 180.0f;
    float gx = (float)(v[4] - d->gyr_offset[1]) * (936.0f * deg_to_rad) / (float)(d->gyr_scale[1] - d->gyr_offset[1]);
    float gy = (float)(v[3] - d->gyr_offset[0]) * (936.0f * deg_to_rad) / (float)(d->gyr_scale[0] - d->gyr_offset[0]);
    float gz = (float)(v[5] - d->gyr_offset[2]) * (936.0f * deg_to_rad) / (float)(d->gyr_scale[2] - d->gyr_offset[2]);

    if (d->is_left) {
        acc_out[0] = ax;
        acc_out[1] = -ay;
        acc_out[2] = -az;
        rot_out[0] = -gx;
        rot_out[1] = gy;
        rot_out[2] = gz;
    } else {
        acc_out[0] = -ax;
        acc_out[1] = -ay;
        acc_out[2] = az;
        rot_out[0] = gx;
        rot_out[1] = gy;
        rot_out[2] = -gz;
    }

    // Match Eden's MotionInput::SetGyroscope (motion_input.cpp:37-59):
    // subtract adaptive resting bias and apply resting deadzone (0.007 rev/s = 0.044 rad/s)
    float raw_rot[3] = {rot_out[0], rot_out[1], rot_out[2]};
    for (int i = 0; i < 3; i++) {
        rot_out[i] = raw_rot[i] - d->gyro_bias[i];
    }
    float rot_len = sqrtf(rot_out[0] * rot_out[0] + rot_out[1] * rot_out[1] + rot_out[2] * rot_out[2]);
    float acc_len = sqrtf(acc_out[0] * acc_out[0] + acc_out[1] * acc_out[1] + acc_out[2] * acc_out[2]);
    if (rot_len < 0.942f && acc_len > 0.9f && acc_len < 1.1f) {
        for (int i = 0; i < 3; i++) {
            d->gyro_bias[i] = d->gyro_bias[i] * 0.9995f + raw_rot[i] * 0.0005f;
        }
    }
    if (rot_len < 0.044f) {
        rot_out[0] = 0.0f;
        rot_out[1] = 0.0f;
        rot_out[2] = 0.0f;
    }
}

static void joycon_report(void *ctx, IOReturn result, void *sender, IOHIDReportType type, uint32_t report_id, uint8_t *r, CFIndex len) {
    (void)sender;
    joycon_device *d = ctx;
    if (result != kIOReturnSuccess || type != kIOHIDReportTypeInput || len < 49 || (report_id != 0x30 && report_id != 0x21)) return;

    if (report_id == 0x21) {
        pthread_mutex_lock(&d->subcmd_lock);
        d->last_subcmd_ack = r[13];
        d->last_subcmd_id = r[14];
        memcpy(d->last_subcmd_data, r + 15, 34);
        d->subcmd_seq++;
        pthread_cond_broadcast(&d->subcmd_cv);
        pthread_mutex_unlock(&d->subcmd_lock);
    }

    uint8_t batt = r[2];
    int batt_chg = (batt >> 4) & 1;
    int batt_lvl = (batt >> 5) & 7;
    if (d->is_left) {
        atomic_store(&g_jc_left_battery, batt_lvl);
        atomic_store(&g_jc_left_charging, batt_chg);
    } else {
        atomic_store(&g_jc_right_battery, batt_lvl);
        atomic_store(&g_jc_right_charging, batt_chg);
    }

    uint64_t now = now_ns();
    int new_imu_pkt = 0;
    if (report_id == 0x30) {
        if (!d->has_pkt_id || d->last_pkt_id != r[1]) {
            d->has_pkt_id = 1;
            d->last_pkt_id = r[1];
            new_imu_pkt = 1;
        }
    }

    if (d->is_left) {
        // Buttons
        uint64_t b = 0;
        if (r[5] & 0x01) b |= DM_BTN_DOWN;
        if (r[5] & 0x02) b |= DM_BTN_UP;
        if (r[5] & 0x04) b |= DM_BTN_RIGHT;
        if (r[5] & 0x08) b |= DM_BTN_LEFT;
        if (r[5] & 0x10) b |= DM_BTN_LEFT_SR;
        if (r[5] & 0x20) b |= DM_BTN_LEFT_SL;
        if (r[5] & 0x40) b |= DM_BTN_L;
        if (r[5] & 0x80) b |= DM_BTN_ZL;
        if (r[4] & 0x01) b |= DM_BTN_MINUS;
        if (r[4] & 0x08) b |= DM_BTN_LS;
        atomic_store(&g_jc_left_buttons, b);

        // Stick
        uint16_t raw_lx = (uint16_t)(r[6] | ((r[7] & 0x0f) << 8));
        uint16_t raw_ly = (uint16_t)((r[7] >> 4) | (r[8] << 4));
        int32_t lx, ly;
        joycon_process_stick(d, raw_lx, raw_ly, &lx, &ly);
        atomic_store(&g_jc_left_lx, lx);
        atomic_store(&g_jc_left_ly, ly);

        if (new_imu_pkt) {
            uint64_t step_ns = 5000000;
            if (!d->imu_ns || now < d->imu_ns || now - d->imu_ns > 60000000) {
                d->imu_ns = now - 3 * step_ns;
            }
            for (int k = 0; k < 3; k++) {
                float acc[3], rot[3];
                joycon_decode_imu(d, r + 13 + 12 * k, acc, rot);
                d->imu_ns += step_ns;
                if (d->imu_ns > now) d->imu_ns = now - (uint64_t)(2 - k) * 1000;
                dm_motion_set_hid_device(0, 0, acc, rot, d->imu_ns);
            }
        }
    } else {
        // Right Joy-Con
        uint64_t b = 0;
        if (r[3] & 0x01) b |= DM_BTN_Y;
        if (r[3] & 0x02) b |= DM_BTN_X;
        if (r[3] & 0x04) b |= DM_BTN_B;
        if (r[3] & 0x08) b |= DM_BTN_A;
        if (r[3] & 0x10) b |= DM_BTN_RIGHT_SR;
        if (r[3] & 0x20) b |= DM_BTN_RIGHT_SL;
        if (r[3] & 0x40) b |= DM_BTN_R;
        if (r[3] & 0x80) b |= DM_BTN_ZR;
        if (r[4] & 0x02) b |= DM_BTN_PLUS;
        if (r[4] & 0x04) b |= DM_BTN_RS;
        atomic_store(&g_jc_right_buttons, b);

        // Stick
        uint16_t raw_rx = (uint16_t)(r[9] | ((r[10] & 0x0f) << 8));
        uint16_t raw_ry = (uint16_t)((r[10] >> 4) | (r[11] << 4));
        int32_t rx, ry;
        joycon_process_stick(d, raw_rx, raw_ry, &rx, &ry);
        atomic_store(&g_jc_right_rx, rx);
        atomic_store(&g_jc_right_ry, ry);

        if (report_id == 0x30) {
            int ring_on = atomic_load(&d->ring_connected);
            if (new_imu_pkt) {
                int imu_frames = ring_on ? 2 : 3;
                uint64_t step_ns = ring_on ? 7500000 : 5000000;
                if (!d->imu_ns || now < d->imu_ns || now - d->imu_ns > 60000000) {
                    d->imu_ns = now - (uint64_t)imu_frames * step_ns;
                }
                for (int k = 0; k < imu_frames; k++) {
                    float acc[3], rot[3];
                    joycon_decode_imu(d, r + 13 + 12 * k, acc, rot);
                    d->imu_ns += step_ns;
                    if (d->imu_ns > now) d->imu_ns = now - (uint64_t)(imu_frames - 1 - k) * 1000;
                    dm_motion_set_hid_device(0, 1, acc, rot, d->imu_ns);
                }
            }
            if (ring_on) {
                int16_t raw_ring = (int16_t)((uint16_t)r[39] | ((uint16_t)r[40] << 8));
                if (raw_ring > 500) {
                    if (d->ring_center == 0) {
                        d->ring_center = raw_ring;
                        d->ring_min = (int16_t)(raw_ring - 800);
                        d->ring_max = (int16_t)(raw_ring + 800);
                        LOG("[hid] Ring-Con calibrated resting value: %d (initial range [%d, %d])", raw_ring, d->ring_min, d->ring_max);
                    }
                    if (raw_ring > d->ring_max) d->ring_max = raw_ring;
                    if (raw_ring < d->ring_min) d->ring_min = raw_ring;
                    float diff = (float)(raw_ring - d->ring_center);
                    float force = 0.0f;
                    if (diff > 0.0f && d->ring_max > d->ring_center) {
                        force = diff / (float)(d->ring_max - d->ring_center);
                    } else if (diff < 0.0f && d->ring_center > d->ring_min) {
                        force = diff / (float)(d->ring_center - d->ring_min);
                    }
                    uint32_t bits;
                    memcpy(&bits, &force, sizeof(bits));
                    atomic_store(&g_jc_ring_force_bits, bits);
                    int bucket = (int)(force * 5.0f);
                    if (bucket != d->ring_log_bucket && (bucket <= -1 || bucket >= 1 || d->ring_log_bucket <= -1 || d->ring_log_bucket >= 1)) {
                        d->ring_log_bucket = bucket;
                        LOG("[hid] Ring-Con input: raw=%d center=%d force=%.2f", raw_ring, d->ring_center, force);
                    }
                }
            }
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
    d->stop_sends = 2;
    for (int i = 0; i < 3; i++) {
        d->acc_scale[i] = 0x4000;
        d->gyr_scale[i] = 0x3be7;
    }
    d->stick_center[0] = 2048; d->stick_center[1] = 2048;
    d->stick_min[0] = 448;     d->stick_min[1] = 448;
    d->stick_max[0] = 3648;    d->stick_max[1] = 3648;
    d->stick_deadzone = 220;
    atomic_store(&d->refs, 1);
    pthread_mutex_init(&d->subcmd_lock, NULL);
    pthread_cond_init(&d->subcmd_cv, NULL);
    pthread_mutex_init(&d->hid_write_lock, NULL);
    pthread_mutex_init(&d->rumble_lock, NULL);
    pthread_cond_init(&d->rumble_cv, NULL);
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

    dm_haptics_route(0, 1);

    IOHIDDeviceRegisterInputReportCallback(hid, d->report, sizeof d->report, joycon_report, d);

    // Start dedicated per-Joy-Con Bluetooth rumble worker
    atomic_fetch_add(&d->refs, 1);
    pthread_t rthr;
    if (pthread_create(&rthr, NULL, joycon_rumble_thread, d) == 0) {
        pthread_detach(rthr);
    } else {
        joycon_release(d);
    }

    // Start synchronous initialization worker (reads SPI calibration, configures IMU & report 0x30, then probes Ring-Con on R)
    joycon_retain(d);
    pthread_t ithr;
    if (pthread_create(&ithr, NULL, joycon_init_thread, d) == 0) {
        pthread_detach(ithr);
    } else {
        joycon_release(d);
    }
}

static void joycon_removed(void *ctx, IOReturn result, void *sender, IOHIDDeviceRef hid) {
    (void)ctx; (void)result; (void)sender;
    joycon_device **link = &g_joycon_devices;
    while (*link && (*link)->hid != hid) link = &(*link)->next;
    if (!*link) return;
    joycon_device *d = *link;
    *link = d->next;

    atomic_store(&d->removed, 1);
    pthread_mutex_lock(&d->subcmd_lock);
    pthread_cond_broadcast(&d->subcmd_cv);
    pthread_mutex_unlock(&d->subcmd_lock);
    pthread_mutex_lock(&d->rumble_lock);
    pthread_cond_broadcast(&d->rumble_cv);
    pthread_mutex_unlock(&d->rumble_lock);

    IOHIDDeviceRegisterInputReportCallback(hid, d->report, sizeof d->report, NULL, NULL);

    if (d->was_rumbling) {
        uint8_t pkt[10] = {0x10, (uint8_t)((d->rumble_counter++) & 0x0F), 0x00, 0x01, 0x40, 0x40, 0x00, 0x01, 0x40, 0x40};
        pthread_mutex_lock(&d->hid_write_lock);
        IOHIDDeviceSetReport(hid, kIOHIDReportTypeOutput, 0x10, pkt, sizeof(pkt));
        pthread_mutex_unlock(&d->hid_write_lock);
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
        atomic_store(&g_jc_left_battery, -1);
        atomic_store(&g_jc_left_charging, 0);
        dm_motion_clear_hid_device(0, 0);
        LOG("[hid] Joy-Con (L) disconnected");
    } else {
        atomic_store(&g_joycon_right_connected, 0);
        atomic_store(&g_jc_right_buttons, 0);
        atomic_store(&g_jc_right_rx, 0);
        atomic_store(&g_jc_right_ry, 0);
        atomic_store(&g_jc_right_battery, -1);
        atomic_store(&g_jc_right_charging, 0);
        atomic_store(&g_jc_ring_connected, 0);
        atomic_store(&g_jc_ring_force_bits, 0);
        dm_motion_clear_hid_device(0, 1);
        LOG("[hid] Joy-Con (R) disconnected");
    }
    joycon_publish_pad();

    if (dm_joycon_count() == 0) {
        dm_haptics_route(0, 0);
    }

    joycon_release(d);
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
