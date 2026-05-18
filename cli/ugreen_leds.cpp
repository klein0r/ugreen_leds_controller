#include "ugreen_leds.h"
#include <string>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
#include <chrono>

#define I2C_DEV_PATH  "/sys/class/i2c-dev/"

ugreen_leds_t::model_t ugreen_leds_t::detect_model() {
    std::ifstream ifs("/sys/class/dmi/id/product_name");
    std::string product;
    std::getline(ifs, product);
    if (product.find("iDX6011") != std::string::npos ||
        product.find("iDX6012") != std::string::npos)
        return model_t::IDX6011;
    return model_t::DXP;
}

void ugreen_leds_t::init_idx6011() {
    // Mirrors the 5-call host-takeover sequence from leds_ugreen_probe() in leds-mcu.ko
    auto send = [&](uint8_t cmd, uint8_t p1, uint8_t p2, uint8_t p3, uint8_t p4) {
        uint16_t sum = 0xa1 + cmd + p1 + p2 + p3 + p4;
        std::vector<uint8_t> data {
            0xa0, 0x01, 0x00, 0x00,
            cmd, p1, p2, p3, p4,
            (uint8_t)((sum >> 8) & 0xff),
            (uint8_t)(sum & 0xff)
        };
        _i2c.write_smbus_block_data(0x00, data);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    };
    send(0x04, 0,   0,   0,   0);
    send(0x01, 255, 0,   0,   0);
    send(0x02, 255, 255, 255, 0);
    send(0x03, 255, 0,   0,   0);
    send(0x01, 255, 0,   0,   0);
}

int ugreen_leds_t::start() {
    namespace fs = std::filesystem;

    if (!fs::exists(I2C_DEV_PATH))
        return -1;

    for (const auto& entry : fs::directory_iterator(I2C_DEV_PATH)) {
        if (entry.is_directory()) {
            std::ifstream ifs(entry.path() / "device/name");
            std::string line;
            std::getline(ifs, line);

            if (line.rfind("SMBus I801 adapter", 0) == 0) {
                const auto i2c_dev = "/dev/" + entry.path().filename().string();
                int rc = _i2c.start(i2c_dev.c_str(), UGREEN_LED_I2C_ADDR);
                if (rc < 0) return rc;
                _model = detect_model();
                if (_model == model_t::IDX6011)
                    init_idx6011();
                return 0;
            }
        }
    }

    return -1;
}

// Returns the I2C write register for a given LED on the current model.
// DXP:     power=0 netdev=1 disk1=2..disk8=9  (netdev2 unsupported)
// iDX6011: power=0 netdev=1 netdev2=2 disk1=3..disk6=8  (disk7/8 unsupported)
uint8_t ugreen_leds_t::get_i2c_reg(led_type_t id) const {
    if (_model == model_t::IDX6011) {
        switch (id) {
            case led_type_t::power:   return 0;
            case led_type_t::netdev:  return 1;
            case led_type_t::netdev2: return 2;
            case led_type_t::disk1:   return 3;
            case led_type_t::disk2:   return 4;
            case led_type_t::disk3:   return 5;
            case led_type_t::disk4:   return 6;
            case led_type_t::disk5:   return 7;
            case led_type_t::disk6:   return 8;
            default: return 0xff; // unsupported
        }
    }
    // DXP: original mapping – enum value directly maps to register
    switch (id) {
        case led_type_t::power:   return 0;
        case led_type_t::netdev:  return 1;
        case led_type_t::disk1:   return 2;
        case led_type_t::disk2:   return 3;
        case led_type_t::disk3:   return 4;
        case led_type_t::disk4:   return 5;
        case led_type_t::disk5:   return 6;
        case led_type_t::disk6:   return 7;
        case led_type_t::disk7:   return 8;
        case led_type_t::disk8:   return 9;
        default: return 0xff; // unsupported (e.g. netdev2 on DXP)
    }
}

static int compute_checksum(const std::vector<uint8_t>& data, int size) {
    if (size < 2 || size > (int)data.size()) 
        return 0;

    int sum = 0;
    for (int i = 0; i < size; ++i)
        sum += (int)data[i];

    return sum;
}

static bool verify_checksum(const std::vector<uint8_t>& data) {
    int size = data.size();
    if (size < 2) return false;
    int sum = compute_checksum(data, size - 2);
    return sum != 0 && sum == (data[size - 1] | (((int)data[size - 2]) << 8));
}

static void append_checksum(std::vector<uint8_t>& data) {
    int size = data.size();
    int sum = compute_checksum(data, size);
    data.push_back((sum >> 8) & 0xff);
    data.push_back(sum & 0xff);
}

ugreen_leds_t::led_data_t ugreen_leds_t::get_status(led_type_t id) {
    led_data_t data { };
    data.is_available = false;

    uint8_t reg = get_i2c_reg(id);
    if (reg == 0xff) return data; // unsupported LED for this model
    auto raw_data = _i2c.read_block_data(0x81 + reg, 0xb);
    if (raw_data.size() != 0xb || !verify_checksum(raw_data)) 
        return data;

    switch (raw_data[0]) {
        case 0: data.op_mode = op_mode_t::off; break;
        case 1: data.op_mode = op_mode_t::on; break;
        case 2: data.op_mode = op_mode_t::blink; break;
        case 3: data.op_mode = op_mode_t::breath; break;
        default: return data;
    };


    data.brightness = raw_data[1];
    data.color_r = raw_data[2];
    data.color_g = raw_data[3];
    data.color_b = raw_data[4];
    int t_hight = (((int)raw_data[5]) << 8) | raw_data[6];
    int t_low = (((int)raw_data[7]) << 8) | raw_data[8];
    data.t_on = t_low;
    data.t_off = t_hight - t_low;
    data.is_available = true;

    return data;
}

int ugreen_leds_t::_change_status(led_type_t id, uint8_t command, std::array<std::optional<uint8_t>, 4> params) {
    uint8_t reg = get_i2c_reg(id);
    if (reg == 0xff) return -1; // unsupported LED for this model

    uint8_t p0 = params[0].value_or(0x00);
    uint8_t p1 = params[1].value_or(0x00);
    uint8_t p2 = params[2].value_or(0x00);
    uint8_t p3 = params[3].value_or(0x00);

    if (_model == model_t::IDX6011) {
        // iDX6011 Pro: SMBus block write – kernel prepends count byte on wire
        // Wire: [0x3a W][reg][count=11][0xa0][0x01][0x00][0x00][cmd][p0][p1][p2][p3][ck_hi][ck_lo]
        uint16_t sum = 0xa1 + command + p0 + p1 + p2 + p3;
        std::vector<uint8_t> data {
            0xa0, 0x01, 0x00, 0x00,
            command, p0, p1, p2, p3,
            (uint8_t)((sum >> 8) & 0xff),
            (uint8_t)(sum & 0xff)
        };
        return _i2c.write_smbus_block_data(reg, data);
    }

    // DXP: I2C block write – no count byte, led_id repeated in data[0]
    // Wire: [0x3a W][reg][reg][0xa0][0x01][0x00][0x00][cmd][p0][p1][p2][p3][ck_hi][ck_lo]
    std::vector<uint8_t> data {
        0x00, 0xa0, 0x01,
        0x00, 0x00, command,
        p0, p1, p2, p3,
    };
    append_checksum(data);
    data[0] = reg;
    return _i2c.write_block_data(reg, data);
}

int ugreen_leds_t::set_onoff(led_type_t id, uint8_t status) {
    if (status >= 2) return -1;
    return _change_status(id, 0x03, { status } );
}

int ugreen_leds_t::_set_blink_or_breath(uint8_t command, led_type_t id, uint16_t t_on, uint16_t t_off) {
    uint16_t t_hight = t_on + t_off;
    uint16_t t_low = t_on;
    return _change_status(id, command, { 
        (uint8_t)(t_hight >> 8), 
        (uint8_t)(t_hight & 0xff), 
        (uint8_t)(t_low >> 8),
        (uint8_t)(t_low & 0xff),
    } );
}

int ugreen_leds_t::set_rgb(led_type_t id, uint8_t r, uint8_t g, uint8_t b) {
    return _change_status(id, 0x02, { r, g, b } );
}

int ugreen_leds_t::set_brightness(led_type_t id, uint8_t brightness) {
    return _change_status(id, 0x01, { brightness } );
}

bool ugreen_leds_t::is_last_modification_successful() {
    return _i2c.read_byte_data(0x80) == 1;
}

int ugreen_leds_t::set_blink(led_type_t id, uint16_t t_on, uint16_t t_off) {
    return _set_blink_or_breath(0x04, id, t_on, t_off);
}

int ugreen_leds_t::set_breath(led_type_t id, uint16_t t_on, uint16_t t_off) {
    return _set_blink_or_breath(0x05, id, t_on, t_off);
}
