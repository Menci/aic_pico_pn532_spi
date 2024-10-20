/*
 * PN532 NFC Reader
 * WHowe <github.com/whowechina>
 *
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "hardware/gpio.h"
#include "hardware/i2c.h"

#include "pn532.h"

#include "FreeRTOS.h"
#include "task.h"

#define DEBUG(...) { if (0) printf(__VA_ARGS__); }

#define IO_TIMEOUT_US 1000
#define PN532_I2C_ADDRESS 0x24

#define PN532_PREAMBLE 0
#define PN532_STARTCODE1 0
#define PN532_STARTCODE2 0xff
#define PN532_POSTAMBLE 0

#define PN532_HOSTTOPN532 0xd4
#define PN532_PN532TOHOST 0xd5

#define STATUS_READ     2
#define DATA_WRITE      1
#define DATA_READ       3

static spi_inst_t *spi_port = spi0;
static uint8_t gpio_nss;
static pn532_wait_loop_t wait_loop = NULL;

static void sleep_ms_rtos(uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));
}

bool pn532_init(spi_inst_t *port, uint8_t nss)
{
    spi_port = port;
    gpio_nss = nss;

    gpio_init(nss);
    gpio_set_dir(nss, GPIO_OUT);
    gpio_pull_up(nss);
    gpio_put(nss, 1);

    // Wake up
    gpio_put(gpio_nss, 0);
    sleep_ms_rtos(2);
    gpio_put(gpio_nss, 1);

    uint32_t ver = pn532_firmware_ver();
    return (ver > 0) && (ver < 0x7fffffff);
}

void pn532_set_wait_loop(pn532_wait_loop_t loop)
{
    wait_loop = loop;
}

static uint8_t reverse_bit(uint8_t x) {
    uint8_t y = 0;
    for (size_t i = 0; i < 8; i++) {
        if (x & (1u << i)) {
            y |= 1u << (8 - i - 1);
        }
    }
    return y;
}

static int pn532_write(const uint8_t *data, uint8_t len)
{
    uint8_t buffer[len + 1];
    buffer[0] = reverse_bit(DATA_WRITE);
    for (int i = 0; i < len; i++) {
        buffer[i + 1] = reverse_bit(data[i]);
    }

    gpio_put(gpio_nss, 0);
    sleep_ms_rtos(2);

    int result = spi_write_blocking(spi_port, buffer, len + 1);

    gpio_put(gpio_nss, 1);
    sleep_ms_rtos(2);

    return result;
}

static void pn532_begin_read(uint8_t mode)
{
    gpio_put(gpio_nss, 0);
    sleep_ms_rtos(2);

    uint8_t mode_data[1] = {reverse_bit(mode)};
    spi_write_blocking(spi_port, mode_data, 1);
}

static int pn532_do_read(uint8_t *data, uint8_t len)
{
    int result = spi_read_blocking(spi_port, 0, data, len);

    for (int i = 0; i < len; i++) {
        data[i] = reverse_bit(data[i]);
    }

    return result;
}

static void pn532_end_read()
{
    gpio_put(gpio_nss, 1);
    sleep_ms_rtos(2);
}

static bool pn532_wait_ready()
{
    for (int retry = 0; retry < 30; retry++) {
        pn532_begin_read(STATUS_READ);
        uint8_t status = 0;
        int result = pn532_do_read(&status, 1);
        pn532_end_read();

        if (result == 1 && status == 0x01) {
            return true;
        }
        if (wait_loop) {
            wait_loop();
        }
        sleep_ms_rtos(1);
    }

    return false;
}

static int read_frame(uint8_t *frame, uint8_t len)
{
    return pn532_do_read(frame, len);
}

static int write_frame(const uint8_t *frame, uint8_t len)
{
    return pn532_write(frame, len);
}

static bool read_ack()
{
    uint8_t resp[6];

    if (!pn532_wait_ready()) {
        return false;
    }

    pn532_begin_read(DATA_READ);
    read_frame(resp, 6);
    pn532_end_read();

    const uint8_t expect_ack[] = {0, 0, 0xff, 0, 0xff, 0};
    if (memcmp(resp, expect_ack, 6) != 0) {
        return false;
    }
   
    return true;
}

int pn532_write_data(const uint8_t *data, uint8_t len)
{
    uint8_t frame[7 + len];
    frame[0] = PN532_PREAMBLE;
    frame[1] = PN532_STARTCODE1;
    frame[2] = PN532_STARTCODE2;
    uint8_t checksum = 0xff;

    frame[3] = len;
    frame[4] = (~len + 1);

    for (int i = 0; i < len; i++) {
        frame[5 + i] = data[i];
        checksum += data[i];
    }

    frame[5 + len] = ~checksum;
    frame[6 + len] = PN532_POSTAMBLE;

    write_frame(frame, 7 + len);

    return read_ack();
}

int pn532_read_data(uint8_t *data, uint8_t len)
{
    uint8_t resp[len + 2];

    int result = read_frame(resp, len + 2);
    if (result != len + 2) {
        return -1;
    }

    uint8_t checksum = 0;
    for (int i = 0; i <= len; i++) {
        data[i] = resp[i];
        checksum += resp[i];
    }

    if (checksum != 0) {
        return -1;
    }

    if (resp[len + 1] != PN532_POSTAMBLE) {
        return -1;
    }
    
    return len;
}

int pn532_write_command(uint8_t cmd, const uint8_t *param, uint8_t len)
{
    uint8_t data[len + 2];
    data[0] = PN532_HOSTTOPN532;
    data[1] = cmd;

    memcpy(data + 2, param, len);

    return pn532_write_data(data, len + 2);
}

int pn532_peak_response_len()
{
    uint8_t buf[5];

    pn532_do_read(buf, 5);
    if (buf[0] != PN532_PREAMBLE ||
        buf[1] != PN532_STARTCODE1 ||
        buf[2] != PN532_STARTCODE2) {
        return -1;
    }

    uint8_t length = buf[3];
    uint8_t length_check = length + buf[4];

    if (length_check != 0) {
        return -1;
    }

    return buf[3];
}

int pn532_read_response(uint8_t cmd, uint8_t *resp, uint8_t len)
{
    if (!pn532_wait_ready()) {
        return -1;
    }

    pn532_begin_read(DATA_READ);

    int real_len = pn532_peak_response_len();
    if (real_len < 0) {
        return -1;
    }

    if (real_len < 2) {
        return -1;
    }

    uint8_t data[real_len];
    int ret = pn532_read_data(data, real_len);
    pn532_end_read();
    if (ret != real_len ||
        data[0] != PN532_PN532TOHOST ||
        data[1] != cmd + 1) {
        return -1;
    }

    int data_len = real_len - 2;
    if (data_len > len) {
        return -1;
    }

    if (data_len <= 0) {
        return -1;
    }
    
    memcpy(resp, data + 2, data_len);

    return data_len;
}

uint32_t pn532_firmware_ver()
{
    int ret = pn532_write_command(0x02, NULL, 0);
    if (ret < 0) {
        return 0;
    }

    uint8_t ver[4];
    int result = pn532_read_response(0x02, ver, sizeof(ver));
    if (result < 4) {
        return 0;
    }

    uint32_t version = 0;
    for (int i = 0; i < 4; i++) {
        version <<= 8;
        version |= ver[i];
    }
    return version;
}

bool pn532_config_rf()
{
    uint8_t param[] = {0x05, 0xff, 0x01, 0x50};
    pn532_write_command(0x32, param, sizeof(param));

    return pn532_read_response(0x32, param, sizeof(param)) == 0;
}

bool pn532_config_sam()
{
    uint8_t param[] = {0x01, 0x14, 0x01};
    pn532_write_command(0x14, param, sizeof(param));

    uint8_t resp;
    return pn532_read_response(0x14, &resp, 1) == 0;
}

static bool pn532_set_rf_field(bool auto_rf, bool on_off)
{
    uint8_t param[] = { 1, (auto_rf ? 2 : 0) | (on_off ? 1 : 0) };
    pn532_write_command(0x32, param, 2);

    uint8_t resp;
    return pn532_read_response(0x32, &resp, 1) >= 0;
}

void pn532_rf_field(bool on)
{
    pn532_set_rf_field(true, on);
    if (on) {
        pn532_config_sam();
    }
}

static uint8_t readbuf[255];

bool pn532_poll_mifare(uint8_t uid[7], int *len)
{
    uint8_t param[] = {0x01, 0x00};
    int ret = pn532_write_command(0x4a, param, sizeof(param));
    if (ret < 0) {
        return false;
    }

    int result = pn532_read_response(0x4a, readbuf, sizeof(readbuf));
    if (result < 1 || readbuf[0] != 1) {
        return false;
    }

    int idlen = readbuf[5];
    if ((idlen > 8) || (result != idlen + 6)) {
        return false;
    }

    memcpy(uid, readbuf + 6, idlen);
    *len = idlen;

    return true;
}

static struct __attribute__((packed)) {
    uint8_t idm[8];
    uint8_t pmm[8];
    uint8_t syscode[2];
    uint8_t inlist_tag;
} felica_poll_cache;

bool pn532_poll_felica(uint8_t uid[8], uint8_t pmm[8], uint8_t syscode[2], bool from_cache)
{
    if (from_cache) {
        memcpy(uid, felica_poll_cache.idm, 8);
        memcpy(pmm, felica_poll_cache.pmm, 8);
        memcpy(syscode, felica_poll_cache.syscode, 2);
        return true;
    }

    uint8_t param[] = { 1, 1, 0, 0xff, 0xff, 1, 0};
    int ret = pn532_write_command(0x4a, param, sizeof(param));
    if (ret < 0) {
        return false;
    }

    int result = pn532_read_response(0x4a, readbuf, sizeof(readbuf));

    if ((result != 22) || (readbuf[0] != 1) || (readbuf[2] != 20)) {
        return false;
    }

    memcpy(&felica_poll_cache, readbuf + 4, 18);
    felica_poll_cache.inlist_tag = readbuf[1];

    memcpy(uid, readbuf + 4, 8);
    memcpy(pmm, readbuf + 12, 8);
    memcpy(syscode, readbuf + 20, 2);

    return true;
}

bool pn532_mifare_auth(const uint8_t uid[4], uint8_t block_id, uint8_t key_id, const uint8_t key[6])
{
    uint8_t param[] = {
        1, key_id ? 0x61 : 0x60, block_id,
        key[0], key[1], key[2], key[3], key[4], key[5],
        uid[0], uid[1], uid[2], uid[3]
    };

    int ret = pn532_write_command(0x40, param, sizeof(param));
    if (ret < 0) {
        DEBUG("\nPN532 failed mifare auth command");
        return false;
    }
    int result = pn532_read_response(0x40, readbuf, sizeof(readbuf));
    if (readbuf[0] != 0) {
        DEBUG("\nPN532 Mifare AUTH failed %d %02x key[%2x:%d]: ", result, readbuf[0], param[1], param[2]);
        for (int i = 0; i < 6; i++) {
            DEBUG("%02x", key[i]);
        }
        return false;
    }

    return true;
}

bool pn532_mifare_read(uint8_t block_id, uint8_t block_data[16])
{
    uint8_t param[] = { 1, 0x30, block_id };

    int ret = pn532_write_command(0x40, param, sizeof(param));
    if (ret < 0) {
        DEBUG("\nPN532 failed mifare read command");
        return false;
    }

    int result = pn532_read_response(0x40, readbuf, sizeof(readbuf));

    if (readbuf[0] != 0 || result != 17) {
        DEBUG("\nPN532 Mifare READ failed %d %02x", result, readbuf[0]);
        return false;
    }

    memmove(block_data, readbuf + 1, 16);

    return true;
}

int pn532_felica_command(uint8_t cmd, const uint8_t *param, uint8_t param_len, uint8_t *outbuf)
{
    int cmd_len = param_len + 11;
    uint8_t cmd_buf[cmd_len + 1];

    cmd_buf[0] = felica_poll_cache.inlist_tag;
    cmd_buf[1] = cmd_len;
    cmd_buf[2] = cmd;
    memcpy(cmd_buf + 3, felica_poll_cache.idm, 8);
    memcpy(cmd_buf + 11, param, param_len);

    int ret = pn532_write_command(0x40, cmd_buf, sizeof(cmd_buf));
    if (ret < 0) {
        DEBUG("\nFailed send felica command");
        return -1;
    }

    int result = pn532_read_response(0x40, readbuf, sizeof(readbuf));

    int outlen = readbuf[1] - 1;
    if ((readbuf[0] & 0x3f) != 0 || result - 2 != outlen) {
        return -1;
    }

    memmove(outbuf, readbuf + 2, outlen);

    return outlen;
}


bool pn532_felica_read(uint16_t svc_code, uint16_t block_id, uint8_t block_data[16])
{
    uint8_t param[] = { 1, svc_code & 0xff, svc_code >> 8,
                        1, block_id >> 8, block_id & 0xff };

    int result = pn532_felica_command(0x06, param, sizeof(param), readbuf);

    if (result != 12 + 16 || readbuf[9] != 0 || readbuf[10] != 0) {
        DEBUG("\nPN532 Felica read failed [%04x:%04x]", svc_code, block_id);
        memset(block_data, 0, 16);
        return true; // we fake the result when it fails
    }

    const uint8_t *result_data = readbuf + 12; 
    memcpy(block_data, result_data, 16);

    return true;
}

bool pn532_felica_write(uint16_t svc_code, uint16_t block_id, const uint8_t block_data[16])
{
    uint8_t param[22] = { 1, svc_code & 0xff, svc_code >> 8,
                        1, block_id >> 8, block_id & 0xff };
    memcpy(param + 6, block_data, 16);
    int result = pn532_felica_command(0x08, param, sizeof(param), readbuf);

    if (result < 0) {
        DEBUG("\nPN532 Felica WRITE failed %d", result);
        return false;
    }

    DEBUG("\nPN532 Felica WRITE success ");
    for (int i = 0; i < result; i++) {
        printf(" %02x", readbuf[i]);
    }
    return false;
}

void pn532_select()
{
    uint8_t ignore_buf[7];
    int ignore_len;
    pn532_poll_mifare(ignore_buf, &ignore_len);
}
