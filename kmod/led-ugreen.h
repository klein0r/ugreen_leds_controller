#ifndef __UGREEN_LED_H
#define __UGREEN_LED_H

#include <linux/types.h>
#include <linux/mutex.h>
#include <linux/leds.h>


#define MODULE_NAME             ( "led-ugreen" )

#define UGREEN_LED_SLAVE_ADDR       ( 0x3a )
#define UGREEN_LED_SLAVE_NAME       ( "led-ugreen" )

#define UGREEN_MAX_LED_NUMBER           ( 10 )
#define UGREEN_LED_CHANGE_STATE_RETRY_COUNT   ( 5 )

enum ugreen_model {
    UGREEN_MODEL_DXP,        // DXP/DX series: i2c_smbus_write_i2c_block_data, led_id in buf[0]
    UGREEN_MODEL_IDX6011,    // iDX6011/iDX6012: i2c_smbus_write_block_data, count byte added by kernel
};

#define UGREEN_LED_STATE_OFF        ( 0 )
#define UGREEN_LED_STATE_ON         ( 1 )
#define UGREEN_LED_STATE_BLINK      ( 2 )
#define UGREEN_LED_STATE_BREATH     ( 3 )
#define UGREEN_LED_STATE_INVALID    ( 4 )

static const char *ugreen_led_state_name[] = { "off", "on", "blink", "breath", "unknown" };

struct ugreen_led_array;

struct ugreen_led_state {
    u8 status;
    u8 r, g, b;
    u8 brightness;
    u16 t_on, t_cycle;

    u8 led_id;
    struct led_classdev cdev;
    struct ugreen_led_array *priv;
};

struct ugreen_led_array {
    struct i2c_client *client;
    struct mutex mutex;
    enum ugreen_model model;
    struct ugreen_led_state state[UGREEN_MAX_LED_NUMBER];
};


#endif // __UGREEN_LED_H
