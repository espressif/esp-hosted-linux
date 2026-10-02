// SPDX-License-Identifier: Apache-2.0
/*
 * ESP32-S31 USB device transport for ESP-Hosted.
 */

#include "sdkconfig.h"

#if !defined(CONFIG_IDF_TARGET_ESP32S31)
#error "ESP USB host interface is supported here only for ESP32-S31"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <rom/rtc.h>
#include "endian.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_fw_version.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "interface.h"
#include "stats.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tusb.h"

#if !defined(CONFIG_TINYUSB_VENDOR_COUNT) || (CONFIG_TINYUSB_VENDOR_COUNT < 1)
#error "ESP32-S31 USB transport requires CONFIG_TINYUSB_VENDOR_COUNT >= 1"
#endif


#define ESP_USB_VENDOR_ITF              0
#define ESP_USB_VENDOR_STR_IDX          4
#define ESP_USB_EP_OUT                  0x02
#define ESP_USB_EP_IN                   0x82
#define ESP_USB_CFG_DESC_LEN            (TUD_CONFIG_DESC_LEN + TUD_VENDOR_DESC_LEN)
#define ESP_USB_FS_MPS                  64
#define ESP_USB_HS_MPS                  512
#define ESP_USB_MAX_XFER                (16 * 1024)
#define ESP_USB_RX_STREAM_CAP           (ESP_USB_MAX_XFER * 2)
#define ESP_USB_RX_QUEUE_DEPTH          32
#define ESP_USB_RX_TASK_STACK           4096
#define ESP_USB_RX_TASK_PRIO            20
#define ESP_USB_TINYUSB_TASK_PRIO       21
#define ESP_USB_RX_WAIT_MS              20
#define ESP_USB_RX_DRAIN_BURST          16
#define ESP_USB_TX_TIMEOUT_MS           1000
#define ESP_USB_SERIAL_STRING_MAX       24

#define ESP_USB_VENDOR_REQ_READY_REPLAY 0xA0
#define ESP_USB_VENDOR_REQ_SOFT_RESET   0xA1
#define ESP_USB_VENDOR_STATUS_OK        0
#define ESP_USB_VENDOR_STATUS_NOT_READY 1
#define ESP_USB_VENDOR_STATUS_UNSUPPORTED 2

static const char *TAG = "FW_USB";

typedef struct {
    bool installed;
    bool running;
    interface_handle_t if_handle;
    QueueHandle_t rx_queue;
    SemaphoreHandle_t rx_sem;
    TaskHandle_t rx_task;
    uint8_t *rx_stream;
    uint8_t *rx_read_buf;
    size_t rx_stream_len;
    size_t ep_mps;
    uint32_t boot_cap;
    bool boot_cap_valid;
    volatile bool ready_replay_pending;
    volatile bool soft_reset_pending;
    volatile bool detached_pending;
} esp_usb_device_ctx_t;

static esp_usb_device_ctx_t s_usb;
static interface_context_t s_context;
static uint32_t s_usb_ctrl_status;
static char s_usb_serial_string[ESP_USB_SERIAL_STRING_MAX] = "ESPHOSTED-UNKNOWN";

static interface_handle_t *esp_usb_init(void);
static int32_t esp_usb_write(interface_handle_t *handle,
                             interface_buffer_handle_t *buf_handle);
static int esp_usb_read(interface_handle_t *handle,
                        interface_buffer_handle_t *buf_handle);
static esp_err_t esp_usb_reset(interface_handle_t *handle);
static void esp_usb_deinit(interface_handle_t *handle);

static if_ops_t s_if_ops = {
    .init = esp_usb_init,
    .write = esp_usb_write,
    .read = esp_usb_read,
    .reset = esp_usb_reset,
    .deinit = esp_usb_deinit,
};

static const uint8_t s_usb_fs_config_desc[] = {
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, ESP_USB_CFG_DESC_LEN, 0x00, 250),
    TUD_VENDOR_DESCRIPTOR(ESP_USB_VENDOR_ITF, ESP_USB_VENDOR_STR_IDX,
                          ESP_USB_EP_OUT, ESP_USB_EP_IN, ESP_USB_FS_MPS),
};

#if TUD_OPT_HIGH_SPEED
static const uint8_t s_usb_hs_config_desc[] = {
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, ESP_USB_CFG_DESC_LEN, 0x00, 250),
    TUD_VENDOR_DESCRIPTOR(ESP_USB_VENDOR_ITF, ESP_USB_VENDOR_STR_IDX,
                          ESP_USB_EP_OUT, ESP_USB_EP_IN, ESP_USB_HS_MPS),
};

static const tusb_desc_device_qualifier_t s_usb_device_qualifier = {
    .bLength = sizeof(tusb_desc_device_qualifier_t),
    .bDescriptorType = TUSB_DESC_DEVICE_QUALIFIER,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_VENDOR_SPECIFIC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .bNumConfigurations = 0x01,
    .bReserved = 0,
};
#endif

static const tusb_desc_device_t s_usb_device_desc = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_VENDOR_SPECIFIC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = 0x303a,
    .idProduct = 0x4002,
    .bcdDevice = 0x0100,
    .iManufacturer = 0x01,
    .iProduct = 0x02,
    .iSerialNumber = 0x03,
    .bNumConfigurations = 0x01,
};

static const char *s_usb_strings[] = {
    (const char[]){ 0x09, 0x04 },
    "Espressif",
    "ESP-Hosted Wi-Fi Transport",
    s_usb_serial_string,
    "ESP-Hosted Vendor Bulk",
};

static void usb_init_serial_string(void)
{
    uint8_t mac[6];

    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK)
        return;

    snprintf(s_usb_serial_string, sizeof(s_usb_serial_string),
             "ESPHOSTED-%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static bool usb_header_valid(const struct esp_payload_header *hdr,
                             size_t *frame_len)
{
    uint16_t offset;
    uint16_t payload_len;

    if (!hdr || hdr->if_type >= ESP_MAX_IF)
        return false;

    offset = le16toh(hdr->offset);
    payload_len = le16toh(hdr->len);
    if (!payload_len || !ESP_OFFSET_VALID(offset))
        return false;

    *frame_len = (size_t)offset + payload_len;
    return *frame_len <= ESP_USB_MAX_XFER;
}

static void usb_free_frame(void *arg)
{
    if (arg)
        heap_caps_free(arg);
}

static bool usb_queue_frame(const uint8_t *frame, size_t frame_len)
{
    interface_buffer_handle_t buf_handle = {0};
    struct esp_payload_header *hdr;
    uint8_t *buf;

    buf = heap_caps_malloc(frame_len, MALLOC_CAP_8BIT);
    if (!buf)
        return false;

    memcpy(buf, frame, frame_len);
    hdr = (struct esp_payload_header *)buf;

    buf_handle.if_type = hdr->if_type;
    buf_handle.if_num = hdr->if_num;
    buf_handle.pkt_type = hdr->packet_type;
    buf_handle.flag = hdr->flags;
    buf_handle.payload = buf;
    buf_handle.payload_len = frame_len;
    buf_handle.priv_buffer_handle = buf;
    buf_handle.free_buf_handle = usb_free_frame;

    if (xQueueSend(s_usb.rx_queue, &buf_handle, 0) != pdTRUE) {
        heap_caps_free(buf);
        return false;
    }

    return true;
}

static void usb_parse_stream(void)
{
    const struct esp_payload_header *hdr;
    size_t frame_len;
    size_t remaining;

    while (s_usb.rx_stream_len >= sizeof(*hdr)) {
        hdr = (const struct esp_payload_header *)s_usb.rx_stream;
        if (!usb_header_valid(hdr, &frame_len)) {
            memmove(s_usb.rx_stream, s_usb.rx_stream + 1,
                    s_usb.rx_stream_len - 1);
            s_usb.rx_stream_len--;
            continue;
        }

        if (s_usb.rx_stream_len < frame_len)
            break;

        if (!usb_queue_frame(s_usb.rx_stream, frame_len))
            ESP_LOGW(TAG, "RX frame queue full, dropping %u bytes",
                     (unsigned)frame_len);

        remaining = s_usb.rx_stream_len - frame_len;
        if (remaining)
            memmove(s_usb.rx_stream, s_usb.rx_stream + frame_len, remaining);
        s_usb.rx_stream_len = remaining;
    }
}

static void usb_ingest(const uint8_t *data, size_t len)
{
    size_t copy;

    while (len) {
        if (s_usb.rx_stream_len == ESP_USB_RX_STREAM_CAP) {
            ESP_LOGW(TAG, "RX stream overflow, dropping partial frame");
            s_usb.rx_stream_len = 0;
        }

        copy = len;
        if (copy > ESP_USB_RX_STREAM_CAP - s_usb.rx_stream_len)
            copy = ESP_USB_RX_STREAM_CAP - s_usb.rx_stream_len;

        memcpy(s_usb.rx_stream + s_usb.rx_stream_len, data, copy);
        s_usb.rx_stream_len += copy;
        data += copy;
        len -= copy;
        usb_parse_stream();
    }
}

static esp_err_t usb_read_available_once(void)
{
    uint32_t available;
    uint32_t read_len;

    available = tud_vendor_n_available(0);
    if (!available)
        return ESP_ERR_TIMEOUT;

    read_len = available;
    if (read_len > ESP_USB_MAX_XFER)
        read_len = ESP_USB_MAX_XFER;

    if (!s_usb.rx_read_buf)
        return ESP_ERR_NO_MEM;

    read_len = tud_vendor_n_read(0, s_usb.rx_read_buf, read_len);
    if (!read_len)
        return ESP_ERR_TIMEOUT;

    usb_ingest(s_usb.rx_read_buf, read_len);
    return ESP_OK;
}

static void usb_wake_rx_task(void)
{
    if (s_usb.rx_sem)
        xSemaphoreGive(s_usb.rx_sem);
}

static esp_err_t usb_write_all(const uint8_t *data, size_t len)
{
    TickType_t deadline;
    size_t sent = 0;

    if (!data || !len)
        return ESP_ERR_INVALID_ARG;

    deadline = xTaskGetTickCount() + pdMS_TO_TICKS(ESP_USB_TX_TIMEOUT_MS);
    while (sent < len) {
        uint32_t available;
        uint32_t chunk;
        uint32_t written;

        if (!tud_vendor_n_mounted(0))
            return ESP_ERR_INVALID_STATE;

#if CFG_TUD_VENDOR_TX_BUFSIZE > 0
        available = tud_vendor_n_write_available(0);
        if (!available) {
            tud_vendor_n_write_flush(0);
            if (xTaskGetTickCount() >= deadline)
                return ESP_ERR_TIMEOUT;
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }
#else
        available = len - sent;
#endif

        chunk = len - sent;
        if (chunk > available)
            chunk = available;

        written = tud_vendor_n_write(0, data + sent, chunk);
        tud_vendor_n_write_flush(0);
        if (!written) {
            if (xTaskGetTickCount() >= deadline)
                return ESP_ERR_TIMEOUT;
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }
        sent += written;
    }

    /*
     * A short logical frame ending exactly on a USB packet boundary gets one
     * pad byte; the host byte-stream parser discards that byte before the
     * next valid header.
     */
    if ((len % s_usb.ep_mps) == 0 && len < ESP_USB_MAX_XFER) {
        const uint8_t pad = 0;

#if CFG_TUD_VENDOR_TX_BUFSIZE > 0
        while (!tud_vendor_n_write_available(0)) {
            tud_vendor_n_write_flush(0);
            if (xTaskGetTickCount() >= deadline)
                return ESP_ERR_TIMEOUT;
            vTaskDelay(pdMS_TO_TICKS(1));
        }
#endif
        if (tud_vendor_n_write(0, &pad, 1) != 1)
            return ESP_FAIL;
        tud_vendor_n_write_flush(0);
    }

    return ESP_OK;
}

static void usb_service_control_requests(void)
{
    if (s_usb.detached_pending) {
        s_usb.detached_pending = false;
        s_usb.rx_stream_len = 0;
    }

    if (s_usb.ready_replay_pending && s_usb.boot_cap_valid) {
        esp_err_t ret = send_bootup_event_to_host(s_usb.boot_cap);

        if (ret != ESP_OK)
            ESP_LOGW(TAG, "READY replay enqueue failed: %s",
                     esp_err_to_name(ret));
    }

    if (s_usb.soft_reset_pending) {
        s_usb.soft_reset_pending = false;
        ESP_LOGW(TAG, "USB soft-reset requested by host");
        vTaskDelay(pdMS_TO_TICKS(50));
        esp_restart();
    }
}

static void usb_rx_task(void *arg)
{
    (void)arg;

    while (s_usb.running) {
        unsigned int drained = 0;

        usb_service_control_requests();

        if (!tud_vendor_n_mounted(0)) {
            xSemaphoreTake(s_usb.rx_sem, pdMS_TO_TICKS(ESP_USB_RX_WAIT_MS));
            continue;
        }

        while (drained < ESP_USB_RX_DRAIN_BURST &&
               usb_read_available_once() == ESP_OK)
            drained++;

        if (!drained)
            xSemaphoreTake(s_usb.rx_sem, pdMS_TO_TICKS(ESP_USB_RX_WAIT_MS));
        else
            taskYIELD();
    }

    s_usb.rx_task = NULL;
    vTaskDelete(NULL);
}

void tud_vendor_rx_cb(uint8_t itf, uint8_t const *buffer,
                      uint16_t bufsize)
{
    (void)itf;
    (void)buffer;
    (void)bufsize;
    usb_wake_rx_task();
}

bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage,
                                tusb_control_request_t const *request)
{
    if (stage != CONTROL_STAGE_SETUP)
        return true;

    if (!request ||
        request->bmRequestType_bit.type != TUSB_REQ_TYPE_VENDOR ||
        request->bmRequestType_bit.recipient != TUSB_REQ_RCPT_INTERFACE ||
        request->bmRequestType_bit.direction != TUSB_DIR_IN)
        return false;

    if ((request->wIndex & 0xff) != ESP_USB_VENDOR_ITF ||
        request->wLength < sizeof(s_usb_ctrl_status))
        return false;

    switch (request->bRequest) {
    case ESP_USB_VENDOR_REQ_READY_REPLAY:
        s_usb.ready_replay_pending = true;
        s_usb_ctrl_status = htole32(ESP_USB_VENDOR_STATUS_OK);
        usb_wake_rx_task();
        break;
    case ESP_USB_VENDOR_REQ_SOFT_RESET:
        s_usb.soft_reset_pending = true;
        s_usb_ctrl_status = htole32(ESP_USB_VENDOR_STATUS_OK);
        usb_wake_rx_task();
        break;
    default:
        s_usb_ctrl_status = htole32(ESP_USB_VENDOR_STATUS_UNSUPPORTED);
        break;
    }

    return tud_control_xfer(rhport, request, &s_usb_ctrl_status,
                            sizeof(s_usb_ctrl_status));
}

static void usb_event_cb(tinyusb_event_t *event, void *arg)
{
    (void)arg;

    if (!event)
        return;

    if (event->id == TINYUSB_EVENT_ATTACHED) {
        ESP_LOGI(TAG, "USB attached");
    } else if (event->id == TINYUSB_EVENT_DETACHED) {
        ESP_LOGW(TAG, "USB detached");
        s_usb.detached_pending = true;
        usb_wake_rx_task();
    }
}

static interface_handle_t *esp_usb_init(void)
{
    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG(usb_event_cb);
    esp_err_t ret;

    if (s_usb.installed)
        return &s_usb.if_handle;

    memset(&s_usb, 0, sizeof(s_usb));
    usb_init_serial_string();

    s_usb.ep_mps = ESP_USB_HS_MPS;
    s_usb.rx_stream = heap_caps_aligned_alloc(64, ESP_USB_RX_STREAM_CAP,
                                              MALLOC_CAP_DMA);
    if (!s_usb.rx_stream)
        goto nomem;

    s_usb.rx_read_buf = heap_caps_aligned_alloc(64, ESP_USB_MAX_XFER,
                                                MALLOC_CAP_DMA);
    if (!s_usb.rx_read_buf)
        goto nomem;

    s_usb.rx_queue = xQueueCreate(ESP_USB_RX_QUEUE_DEPTH,
                                  sizeof(interface_buffer_handle_t));
    if (!s_usb.rx_queue)
        goto nomem;

    s_usb.rx_sem = xSemaphoreCreateBinary();
    if (!s_usb.rx_sem)
        goto nomem;

    tusb_cfg.descriptor.device = &s_usb_device_desc;
    tusb_cfg.descriptor.string = s_usb_strings;
    tusb_cfg.descriptor.string_count =
        sizeof(s_usb_strings) / sizeof(s_usb_strings[0]);
    tusb_cfg.descriptor.full_speed_config = s_usb_fs_config_desc;
#if TUD_OPT_HIGH_SPEED
    tusb_cfg.descriptor.qualifier = &s_usb_device_qualifier;
    tusb_cfg.descriptor.high_speed_config = s_usb_hs_config_desc;
#endif
    tusb_cfg.task.priority = ESP_USB_TINYUSB_TASK_PRIO;

    ret = tinyusb_driver_install(&tusb_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "tinyusb_driver_install failed: %s",
                 esp_err_to_name(ret));
        goto fail;
    }

    s_usb.running = true;
    if (xTaskCreate(usb_rx_task, "usb_rx", ESP_USB_RX_TASK_STACK, NULL,
                    ESP_USB_RX_TASK_PRIO, &s_usb.rx_task) != pdPASS) {
        s_usb.running = false;
        tinyusb_driver_uninstall();
        goto nomem;
    }

    s_usb.installed = true;
    s_usb.if_handle.state = INIT;
    ESP_LOGI(TAG, "ESP32-S31 USB transport ready (max=%u mps=%u)",
             ESP_USB_MAX_XFER, (unsigned)s_usb.ep_mps);
    return &s_usb.if_handle;

nomem:
    ret = ESP_ERR_NO_MEM;
fail:
    if (s_usb.rx_sem)
        vSemaphoreDelete(s_usb.rx_sem);
    if (s_usb.rx_queue)
        vQueueDelete(s_usb.rx_queue);
    if (s_usb.rx_read_buf)
        heap_caps_free(s_usb.rx_read_buf);
    if (s_usb.rx_stream)
        heap_caps_free(s_usb.rx_stream);
    memset(&s_usb, 0, sizeof(s_usb));
    ESP_LOGE(TAG, "USB transport init failed: %s", esp_err_to_name(ret));
    return NULL;
}

static int32_t esp_usb_write(interface_handle_t *handle,
                             interface_buffer_handle_t *buf_handle)
{
    struct esp_payload_header *hdr;
    uint8_t *frame;
    size_t frame_len;
    esp_err_t ret;

    if (!handle || !buf_handle || !buf_handle->payload ||
        !buf_handle->payload_len)
        return ESP_FAIL;

    frame_len = sizeof(*hdr) + buf_handle->payload_len;
    if (frame_len > ESP_USB_MAX_XFER) {
        ESP_LOGE(TAG, "USB frame too large: %u", (unsigned)frame_len);
        return ESP_FAIL;
    }

    frame = heap_caps_aligned_alloc(4, frame_len, MALLOC_CAP_DMA);
    if (!frame)
        return ESP_ERR_NO_MEM;

    memset(frame, 0, sizeof(*hdr));
    hdr = (struct esp_payload_header *)frame;
    hdr->if_type = buf_handle->if_type;
    hdr->if_num = buf_handle->if_num;
    hdr->flags = buf_handle->flag;
    hdr->packet_type = buf_handle->pkt_type;
    hdr->len = htole16(buf_handle->payload_len);
    hdr->offset = htole16(sizeof(*hdr));
    if (hdr->if_type == ESP_TEST_IF)
        debug_raw_tp_set_seq(hdr, buf_handle->raw_tp_seq);

    memcpy(frame + sizeof(*hdr), buf_handle->payload, buf_handle->payload_len);
    ret = usb_write_all(frame, frame_len);
    heap_caps_free(frame);

    return ret == ESP_OK ? (int32_t)buf_handle->payload_len : ESP_FAIL;
}

static int esp_usb_read(interface_handle_t *handle,
                        interface_buffer_handle_t *buf_handle)
{
    if (!handle || !buf_handle || !s_usb.rx_queue)
        return ESP_FAIL;

    if (xQueueReceive(s_usb.rx_queue, buf_handle, pdMS_TO_TICKS(10)) != pdTRUE)
        return 0;

    return buf_handle->payload_len;
}

static esp_err_t esp_usb_reset(interface_handle_t *handle)
{
    if (!handle)
        return ESP_ERR_INVALID_ARG;

    s_usb.soft_reset_pending = true;
    usb_wake_rx_task();
    return ESP_OK;
}

static void esp_usb_deinit(interface_handle_t *handle)
{
    interface_buffer_handle_t pending;

    (void)handle;
    if (!s_usb.installed)
        return;

    s_usb.running = false;
    usb_wake_rx_task();
    for (int i = 0; i < 20 && s_usb.rx_task; i++)
        vTaskDelay(pdMS_TO_TICKS(10));

    tinyusb_driver_uninstall();

    if (s_usb.rx_queue) {
        while (xQueueReceive(s_usb.rx_queue, &pending, 0) == pdTRUE) {
            if (pending.free_buf_handle && pending.priv_buffer_handle)
                pending.free_buf_handle(pending.priv_buffer_handle);
        }
        vQueueDelete(s_usb.rx_queue);
    }
    if (s_usb.rx_sem)
        vSemaphoreDelete(s_usb.rx_sem);
    if (s_usb.rx_read_buf)
        heap_caps_free(s_usb.rx_read_buf);
    if (s_usb.rx_stream)
        heap_caps_free(s_usb.rx_stream);

    memset(&s_usb, 0, sizeof(s_usb));
}

interface_context_t *interface_insert_driver(int (*event_handler)(uint8_t val))
{
    memset(&s_context, 0, sizeof(s_context));
    s_context.type = USB;
    s_context.if_ops = &s_if_ops;
    s_context.event_handler = event_handler;
    ESP_LOGI(TAG, "Using ESP32-S31 USB interface");
    return &s_context;
}

int interface_remove_driver(void)
{
    esp_usb_deinit(&s_usb.if_handle);
    memset(&s_context, 0, sizeof(s_context));
    return 0;
}

esp_err_t send_bootup_event_to_host(uint32_t cap)
{
    struct esp_internal_bootup_event *event;
    struct fw_data *fw_p;
    interface_buffer_handle_t buf_handle = {0};
    uint8_t *pos;
    uint16_t len = 0;
    uint32_t cap_le;
    uint32_t rx_buf_sz;
    BaseType_t queued;

    s_usb.boot_cap = cap;
    s_usb.boot_cap_valid = true;

    buf_handle.payload = heap_caps_malloc(ESP_USB_MAX_XFER, MALLOC_CAP_8BIT);
    if (!buf_handle.payload)
        return ESP_ERR_NO_MEM;
    memset(buf_handle.payload, 0, ESP_USB_MAX_XFER);

    buf_handle.priv_buffer_handle = buf_handle.payload;
    buf_handle.free_buf_handle = heap_caps_free;
    buf_handle.if_type = ESP_INTERNAL_IF;
    buf_handle.if_num = 0;
    buf_handle.pkt_type = PACKET_TYPE_EVENT;

    event = (struct esp_internal_bootup_event *)buf_handle.payload;
    event->header.event_code = ESP_INTERNAL_BOOTUP_EVENT;
    event->header.status = 0;
    pos = event->data;

    *pos++ = ESP_BOOTUP_FIRMWARE_CHIP_ID;
    *pos++ = LENGTH_1_BYTE;
    *pos++ = CONFIG_IDF_FIRMWARE_CHIP_ID;
    len += 3;

    *pos++ = ESP_BOOTUP_CAPABILITY;
    *pos++ = LENGTH_4_BYTE;
    cap_le = htole32(cap);
    memcpy(pos, &cap_le, sizeof(cap_le));
    pos += sizeof(cap_le);
    len += 2 + sizeof(cap_le);

    *pos++ = ESP_BOOTUP_RX_BUF_SIZE;
    *pos++ = LENGTH_4_BYTE;
    rx_buf_sz = htole32(ESP_USB_MAX_XFER);
    memcpy(pos, &rx_buf_sz, sizeof(rx_buf_sz));
    pos += sizeof(rx_buf_sz);
    len += 2 + sizeof(rx_buf_sz);

    *pos++ = ESP_BOOTUP_FW_DATA;
    *pos++ = sizeof(struct fw_data);
    len += 2;
    fw_p = (struct fw_data *)pos;
    fw_p->last_reset_reason = htole32(rtc_get_reset_reason(0));
    memcpy(fw_p->version.project_name, PROJECT_NAME, strlen(PROJECT_NAME));
    fw_p->version.project_name[strlen(PROJECT_NAME)] = '\0';
    fw_p->version.major1 = PROJECT_VERSION_MAJOR_1;
    fw_p->version.major2 = PROJECT_VERSION_MAJOR_2;
    fw_p->version.minor = PROJECT_VERSION_MINOR;
    fw_p->version.revision_patch_1 = PROJECT_REVISION_PATCH_1;
    fw_p->version.revision_patch_2 = PROJECT_REVISION_PATCH_2;
    pos += sizeof(struct fw_data);
    len += sizeof(struct fw_data);

    event->len = len;
    event->header.len = htole16(len + 1);
    buf_handle.payload_len = len + sizeof(struct esp_internal_bootup_event);

    queued = send_to_host(PRIO_Q_HIGH, &buf_handle);
    if (queued != pdTRUE) {
        heap_caps_free(buf_handle.payload);
        return ESP_FAIL;
    }

    /* A successfully queued boot event also satisfies an earlier READY
     * replay request, avoiding a duplicate incarnation event. */
    s_usb.ready_replay_pending = false;
    return ESP_OK;
}
