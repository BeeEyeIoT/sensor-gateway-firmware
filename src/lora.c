#include "lora.h"
#include "display.h"
#include "messages.h"
#include "influx_sender.h"

#include <zephyr/logging/log.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/lora.h>
#include <errno.h>

LOG_MODULE_DECLARE(app);

const struct device *dev_lora = DEVICE_DT_GET(DT_NODELABEL(lora));

#define LORA_THREAD_STACK_SIZE 8192
#define UI_THREAD_PRIORITY   3
static K_THREAD_STACK_DEFINE(lora_stack, LORA_THREAD_STACK_SIZE);
static struct k_thread lora_thread;
static void lora_worker(void *arg1, void *arg2, void *arg3);

int lora_start(void) {
    struct lora_modem_config config = {
        .frequency = 868100000,  
        .bandwidth = BW_125_KHZ, 
        .datarate = SF_9,       
        .preamble_len = 8,
        .coding_rate = CR_4_7,
        .tx_power = 10,         
        .iq_inverted = false,
        .public_network = false,
        .tx = false
    };

    if(!device_is_ready(dev_lora)) {
        LOG_ERR("SX1262 not ready");
        return -ENODEV;
    }

    int ret = lora_config(dev_lora, &config);
    if (ret < 0) {
        LOG_ERR("LoRa config failed: %d", ret);
        return ret;
    }

    LOG_INF("Starting LoRa thread");
    k_thread_create(&lora_thread,
              lora_stack,
              K_THREAD_STACK_SIZEOF(lora_stack),
              lora_worker,
              NULL, NULL, NULL,
              UI_THREAD_PRIORITY,
              0,
              K_NO_WAIT);

    return 0;
}

static void process_measures(const struct measure *measures, uint8_t count) {
    const struct measure *current = measures;
    for (int i=0; i<count; ++i, ++current) {
        display_printf("%02X%02X%02X%02X%02X%02X%02X%02X ",
            current->sensor_id[0],
            current->sensor_id[1],
            current->sensor_id[2],
            current->sensor_id[3],
            current->sensor_id[4],
            current->sensor_id[5],
            current->sensor_id[6],
            current->sensor_id[7]
        );

        switch (current->type) {
            case MEASURE_TYPE_BATTERY:
                display_printf("bat=%dmV\n", current->data.bat.mV);
                break;
            case MEASURE_TYPE_TEMPERATURE:
                display_printf("t=%.1f\n", (double)current->data.th.tempC);
                break;
            case MEASURE_TYPE_TEMP_AND_HUMIDITY:
                display_printf("t=%.1f, h=%.1f%%\n", 
                    (double)current->data.th.tempC, (double)current->data.th.hum);
                break;
            case MEASURE_TYPE_WEIGHT:
                display_printf("w=%.1f\n", (double)current->data.w.weight);
                break;
            default:
                display_printf("Unsupported measure type %d\n", current->type);
        }
    }

    int ret = influx_post_measures(measures, count);
    if (ret) {
        LOG_ERR("Posting to InfluxDB Failed: %d", ret);
        display_printf("Posting failed: %d\n", ret);
    } else {
        LOG_INF("Posted to InfluxDB");
    }
}

static void lora_worker(void *arg1, void *arg2, void *arg3) {
    ARG_UNUSED(arg1);
    ARG_UNUSED(arg2);
    ARG_UNUSED(arg3);
    LOG_INF("LoRa thread started");
    uint8_t buf[256];
    int16_t rssi;
    int8_t snr;

    while (1) {
        int ret = lora_recv(dev_lora, buf, sizeof(buf) - 1, K_FOREVER, &rssi, &snr);
        
        if (ret == -EAGAIN) continue;

        if (ret < 0) {
            LOG_ERR("lora_recv failed: %d", ret);
            continue;
        }

        buf[ret] = 0;

        display_printf("RX (%d bytes) RSSI=%d SNR=%d\n", ret, rssi, snr);

        const uint8_t payload_size = ret;
        const struct message_header *p_header = (const struct message_header*)buf;
        const char *p_payload = buf + sizeof(struct message_header);
        uint32_t magic = *((const uint32_t*)p_header->magic);
        if (magic != MAGIC_BEE_EYE) {
            display_printf("Invalid magic 0x%lx\n", magic);
            continue;
        }

        switch (p_header->type) {
            case MESSAGE_TYPE_TEXT:
                display_printf("%s\n", p_payload);
                break;
            case MESSAGE_TYPE_MEASURE:
                const uint8_t measure_count = (payload_size- sizeof(struct message_header)) / sizeof(struct measure);
                process_measures((const struct measure*)p_payload, measure_count);
                break;
            default:
                display_printf("Invalid message type %u\n", p_header->type);
                break;
        }
    }

}

