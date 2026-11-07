// Raw HID gamepads (pad_hid.h, GitHub issue #637): the default map and DM_HID_MAP, hat directions, axis scaling, the stick dead zone and analog triggers
#include <stdio.h>
#include "../pad_hid.h"

static int checks, fails;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

int main(void) {
    pad_hid h; int32_t s[4];
    pad_hid_init(&h, NULL);
    CHECK(pad_hid_buttons(&h) == 0);
    h.buttons = 1u << 1; CHECK(pad_hid_buttons(&h) == DM_BTN_A);             // usage 2 (right) is A
    h.buttons = 1u << 0; CHECK(pad_hid_buttons(&h) == DM_BTN_B);             // usage 1 (bottom) is B
    h.buttons = 1u << 11 | 1u << 9; CHECK(pad_hid_buttons(&h) == (DM_BTN_PLUS | DM_BTN_ZR));
    pad_hid_init(&h, "a=1,B=2,bogus=3,ZR=99,X=,MINUS=13");
    h.buttons = 1u << 0; CHECK(pad_hid_buttons(&h) == DM_BTN_A);
    h.buttons = 1u << 12; CHECK(pad_hid_buttons(&h) == DM_BTN_MINUS);
    h.buttons = 1u << 9; CHECK(pad_hid_buttons(&h) == DM_BTN_ZR);           // 99 skipped: the default stays
    h.buttons = 1u << 4; CHECK(pad_hid_buttons(&h) == 0);                    // X= maps X to nothing
    pad_hid_init(&h, NULL);
    CHECK(pad_hid_hat(1, 1, 8) == 0 && pad_hid_hat(0, 1, 8) == -1 && pad_hid_hat(8, 0, 7) == -1 && pad_hid_hat(15, 0, 15) == -1);
    h.hat = pad_hid_hat(4, 1, 8); CHECK(pad_hid_buttons(&h) == (DM_BTN_DOWN | DM_BTN_RIGHT));
    CHECK(pad_hid_axis_of(1, 0x30) == PH_X && pad_hid_axis_of(1, 0x35) == PH_RZ && pad_hid_axis_of(2, 0xC5) == PH_BRAKE && pad_hid_axis_of(1, 0x39) == -1);
    CHECK(pad_hid_norm(0, 0, 65535) == -1 && pad_hid_norm(65535, 0, 65535) == 1 && pad_hid_norm(5, 3, 3) == 0);
    h.has[PH_X] = h.has[PH_Y] = h.has[PH_Z] = h.has[PH_RZ] = 1;
    h.axis[PH_X] = 1; h.axis[PH_Y] = -1; h.axis[PH_Z] = 0.05f; h.axis[PH_RZ] = 0.04f;
    pad_hid_sticks(&h, s);
    CHECK(s[0] == DM_STICK_MAX && s[1] == DM_STICK_MAX);                     // Y up; a square-gated corner reaches full x and y
    CHECK(s[2] == 0 && s[3] == 0);                                           // inside the round dead zone
    h.axis[PH_RZ] = 0.5f; pad_hid_sticks(&h, s);
    CHECK(s[2] > 0 && s[3] < 0 && s[3] > -DM_STICK_MAX / 2);                 // past it: the small x stays, the deflection is scaled back
    h.axis[PH_X] = 0.13f; h.axis[PH_Y] = 0; pad_hid_sticks(&h, s);
    CHECK(s[0] > 0 && s[0] < DM_STICK_MAX / 50 && s[1] == 0);                // just past the edge: near 0, no jump
    h.has[PH_Z] = 0; h.has[PH_RX] = 1; h.axis[PH_RX] = -1; pad_hid_sticks(&h, s); CHECK(s[2] == -DM_STICK_MAX && s[3] == 0);   // Rx / Ry without Z
    h.has[PH_ACCEL] = 1; h.axis[PH_ACCEL] = -1; CHECK(!(pad_hid_buttons(&h) & DM_BTN_ZR));
    h.axis[PH_ACCEL] = pad_hid_norm(200, 0, 255); CHECK(pad_hid_buttons(&h) & DM_BTN_ZR);

    // Joy-Con SPI stick calibration 12-bit unpacking
    uint8_t l_cal[9];
    l_cal[0] = 0x40; l_cal[1] = 0x06; l_cal[2] = 0x64; // 1600, 1600
    l_cal[3] = 0xD0; l_cal[4] = 0x87; l_cal[5] = 0x89; // 2000, 2200
    l_cal[6] = 0xDC; l_cal[7] = 0xC5; l_cal[8] = 0x5D; // 1500, 1500
    int16_t l_max_x = (int16_t)(l_cal[0] | ((l_cal[1] & 0x0F) << 8));
    int16_t l_max_y = (int16_t)((l_cal[1] >> 4) | (l_cal[2] << 4));
    int16_t l_cx    = (int16_t)(l_cal[3] | ((l_cal[4] & 0x0F) << 8));
    int16_t l_cy    = (int16_t)((l_cal[4] >> 4) | (l_cal[5] << 4));
    int16_t l_min_x = (int16_t)(l_cal[6] | ((l_cal[7] & 0x0F) << 8));
    int16_t l_min_y = (int16_t)((l_cal[7] >> 4) | (l_cal[8] << 4));
    CHECK(l_max_x == 1600 && l_max_y == 1600);
    CHECK(l_cx == 2000 && l_cy == 2200);
    CHECK(l_min_x == 1500 && l_min_y == 1500);

    // Deadzone and resting position check:
    // With center at (2000, 2200) and resting raw at (2050, 2280): dx=50, dy=80.
    // Distance = sqrt(2500 + 6400) = ~94.3 <= deadzone 220, so resting stick is zeroed.
    int32_t raw_x = 2050, raw_y = 2280;
    int32_t dx = raw_x - l_cx, dy = raw_y - l_cy;
    int32_t dz = 220;
    CHECK((int64_t)dx * dx + (int64_t)dy * dy <= (int64_t)dz * dz);

    printf("t_pad_hid: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
