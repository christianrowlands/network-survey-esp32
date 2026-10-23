// OTA transfer: Control commands (link task), chunk writes (NimBLE host task,
// queued) and the OTA task that writes flash and hashes the image. ota_sm.c
// decides what to do with each chunk; this file does the IDF work under one
// lock.

#include "ota.h"

#include <string.h>

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/message_buffer.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "gatt_server.h"
#include "link.h"
#include "ota_internal.h"
#include "ota_sm.h"
#include "psa/crypto.h"

static const char *TAG = "ota";

#define WINDOW_BYTES 8192
// Room for a full window of small chunks with their headers, plus resends.
#define QUEUE_BYTES (2 * WINDOW_BYTES + 2048)
// Largest OTA write: ATT_MTU 517 - 3.
#define WRITE_MAX 514
// Queued message: generation, connection (2 bytes), then the write.
#define MSG_HEADER 3
#define TASK_STACK 4096
#define TASK_PRIO 4
#define END_DRAIN_MS 3000
#define RESTART_DELAY_US (1000 * 1000)

#define RC_OK ns_board_v1_ResultCode_RESULT_CODE_OK
#define RC_ERROR ns_board_v1_ResultCode_RESULT_CODE_ERROR
#define RC_INVALID_ARGUMENT ns_board_v1_ResultCode_RESULT_CODE_INVALID_ARGUMENT
#define RC_INVALID_STATE ns_board_v1_ResultCode_RESULT_CODE_INVALID_STATE
#define RC_VERIFY_FAILED ns_board_v1_ResultCode_RESULT_CODE_OTA_VERIFY_FAILED

static SemaphoreHandle_t s_lock;  // everything below except the host-task fields
static ota_sm_t s_sm;
static esp_ota_handle_t s_handle;
static const esp_partition_t *s_part;
static psa_hash_operation_t s_hash = PSA_HASH_OPERATION_INIT;
static bool s_in_ota_state;
static bool s_restarting;
static ota_active_cb_t s_on_active;

// Created by the first OtaBegin and kept (the host task may hold the pointer).
static MessageBufferHandle_t volatile s_queue;
static TaskHandle_t s_task;
// Bumped when a transfer is discarded, restarted or resumed; queued chunks of
// an older generation are dropped. 8 bits is plenty: a chunk spends well under
// a second in the queue.
static volatile uint8_t s_gen;
static volatile uint32_t s_queued;  // host task
static volatile uint32_t s_done;    // OTA task

static void send_ack(uint16_t conn, uint32_t next_offset, ns_board_v1_OtaAckStatus status)
{
    ns_board_v1_OtaAck ack = {.next_offset = next_offset, .status = status};
    if (!link_send_ota_ack(conn, &ack)) {
        ESP_LOGW(TAG, "ack %d at %u not queued", status, (unsigned)next_offset);
    }
}

static void set_ota_state(bool on)
{
    if (s_in_ota_state == on) {
        return;
    }
    s_in_ota_state = on;
    link_set_ota_active(on);
    if (s_on_active != NULL) {
        s_on_active(on);
    }
}

// Frees the partial image after the state machine has ended the transfer.
// Caller holds the lock.
static void release(const char *why)
{
    if (s_handle != 0) {
        esp_ota_abort(s_handle);
        s_handle = 0;
    }
    psa_hash_abort(&s_hash);
    s_gen++;
    set_ota_state(false);
    ESP_LOGI(TAG, "transfer discarded: %s", why);
}

static void discard(const char *why)
{
    ota_sm_abort(&s_sm);
    release(why);
}

// ---------------------------------------------------------------------------
// Chunks

// NimBLE host task: queue the write for the OTA task, never block.
static void on_write(uint16_t conn, const uint8_t *data, uint16_t len)
{
    static uint8_t msg[MSG_HEADER + WRITE_MAX];
    static bool boot_not_active_sent;
    MessageBufferHandle_t queue = s_queue;
    if (queue == NULL) {
        // No OtaBegin since boot.
        if (!boot_not_active_sent) {
            boot_not_active_sent = true;
            send_ack(conn, 0, ns_board_v1_OtaAckStatus_OTA_ACK_STATUS_NOT_ACTIVE);
        }
        return;
    }
    if (len > WRITE_MAX) {
        return;
    }
    msg[0] = s_gen;
    msg[1] = (uint8_t)conn;
    msg[2] = (uint8_t)(conn >> 8);
    memcpy(msg + MSG_HEADER, data, len);
    // A full queue drops the chunk; the state machine reports the gap.
    if (xMessageBufferSend(queue, msg, MSG_HEADER + len, 0) != 0) {
        s_queued++;
    }
}

static void handle_chunk(const uint8_t *msg, size_t len)
{
    if (msg[0] != s_gen || s_restarting) {
        return;
    }
    uint16_t conn = (uint16_t)(msg[1] | msg[2] << 8);
    const uint8_t *bytes = NULL;
    size_t n = 0;
    uint32_t at = s_sm.next_offset;
    switch (ota_sm_chunk(&s_sm, msg + MSG_HEADER, len - MSG_HEADER, &bytes, &n, link_mono_ms())) {
    case OTA_SM_CHUNK_WRITE: {
        esp_err_t err = esp_ota_write(s_handle, bytes, n);
        if (err == ESP_OK && psa_hash_update(&s_hash, bytes, n) != PSA_SUCCESS) {
            err = ESP_FAIL;
        }
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "write at %u failed: %s", (unsigned)at, esp_err_to_name(err));
            discard("write failed");
            send_ack(conn, at, ns_board_v1_OtaAckStatus_OTA_ACK_STATUS_WRITE_FAILED);
        } else if (ota_sm_written(&s_sm, n)) {
            send_ack(conn, s_sm.next_offset, ns_board_v1_OtaAckStatus_OTA_ACK_STATUS_OK);
        }
        break;
    }
    case OTA_SM_CHUNK_ACK:
        send_ack(conn, at, ns_board_v1_OtaAckStatus_OTA_ACK_STATUS_OK);
        break;
    case OTA_SM_CHUNK_OUT_OF_ORDER:
        send_ack(conn, at, ns_board_v1_OtaAckStatus_OTA_ACK_STATUS_OUT_OF_ORDER);
        break;
    case OTA_SM_CHUNK_OVERFLOW:
        release("chunk past the image size");
        send_ack(conn, at, ns_board_v1_OtaAckStatus_OTA_ACK_STATUS_OVERFLOW);
        break;
    case OTA_SM_CHUNK_NOT_ACTIVE:
        send_ack(conn, 0, ns_board_v1_OtaAckStatus_OTA_ACK_STATUS_NOT_ACTIVE);
        break;
    case OTA_SM_CHUNK_IGNORE:
        break;
    }
}

static void ota_task(void *arg)
{
    (void)arg;
    static uint8_t msg[MSG_HEADER + WRITE_MAX];
    for (;;) {
        size_t len = xMessageBufferReceive(s_queue, msg, sizeof(msg), pdMS_TO_TICKS(1000));
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (len > MSG_HEADER) {
            handle_chunk(msg, len);
        }
        if (ota_sm_idle_expired(&s_sm, link_mono_ms())) {
            // Leaves the OTA state from this task; link only reads the state.
            release("idle for 5 minutes");
        }
        if (len > 0) {
            s_done++;
        }
        xSemaphoreGive(s_lock);
    }
}

// ---------------------------------------------------------------------------
// Commands (link task)

static void fail(ns_board_v1_ControlResponse *resp, ns_board_v1_ResultCode code, const char *msg)
{
    resp->result = code;
    strlcpy(resp->message, msg, sizeof(resp->message));
}

static bool start_worker(void)
{
    if (s_queue == NULL) {
        MessageBufferHandle_t queue = xMessageBufferCreate(QUEUE_BYTES);
        if (queue == NULL) {
            return false;
        }
        // Kept from now on: the host task may hold the pointer.
        s_queue = queue;
    }
    if (s_task == NULL && xTaskCreate(ota_task, "ota", TASK_STACK, NULL, TASK_PRIO, &s_task) != pdPASS) {
        s_task = NULL;
        return false;
    }
    return true;
}

static void on_begin(uint16_t conn, const ns_board_v1_ControlRequest *req, ns_board_v1_ControlResponse *resp)
{
    const ns_board_v1_OtaBegin *begin = &req->command.ota_begin;
    if (!start_worker()) {
        fail(resp, RC_ERROR, "out of memory");
        return;
    }
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (part == NULL) {
        fail(resp, RC_ERROR, "no update partition");
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_restarting) {
        fail(resp, RC_INVALID_STATE, "restarting");
        goto out;
    }
    bool had_image = s_sm.active;
    switch (ota_sm_begin(&s_sm, begin->image_size, begin->sha256, part->size, link_mono_ms())) {
    case OTA_SM_BEGIN_INVALID:
        fail(resp, RC_INVALID_ARGUMENT, "image size");
        goto out;
    case OTA_SM_BEGIN_FRESH: {
        if (had_image && s_handle != 0) {
            esp_ota_abort(s_handle);
            s_handle = 0;
            psa_hash_abort(&s_hash);
        }
        s_gen++;
        // esp_ota_begin refuses to run from an image still pending
        // verification. Confirm it before the other slot (the rollback
        // target) is erased.
        ota_confirm("OtaBegin");
        // Erase sector by sector while writing (a 1 MB erase up front would
        // stall the radio for seconds).
        esp_err_t err = esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &s_handle);
        if (err == ESP_OK && psa_hash_setup(&s_hash, PSA_ALG_SHA_256) != PSA_SUCCESS) {
            err = ESP_FAIL;
        }
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "begin failed: %s", esp_err_to_name(err));
            discard("begin failed");
            fail(resp, RC_ERROR, esp_err_to_name(err));
            goto out;
        }
        s_part = part;
        ESP_LOGI(TAG, "update of %u bytes into %s", (unsigned)begin->image_size, part->label);
        break;
    }
    case OTA_SM_BEGIN_RESUME:
        s_gen++;  // drop chunks still queued from before the disconnect
        ESP_LOGI(TAG, "update resumes at %u of %u", (unsigned)s_sm.next_offset, (unsigned)s_sm.image_size);
        break;
    }
    set_ota_state(true);
    uint16_t mtu = gatt_server_mtu(conn);
    if (mtu < 23) {
        mtu = 23;  // the link just dropped; the phone will not use the answer
    }
    uint32_t max_chunk = (mtu > WRITE_MAX + 3 ? WRITE_MAX : mtu - 3) - OTA_SM_OFFSET_LEN;
    resp->result = RC_OK;
    resp->which_payload = ns_board_v1_ControlResponse_ota_begin_tag;
    resp->payload.ota_begin = (ns_board_v1_OtaBeginResult){
        .max_chunk_size = max_chunk,
        .window_bytes = WINDOW_BYTES,
        .start_offset = s_sm.next_offset,
    };
out:
    xSemaphoreGive(s_lock);
}

static void restart(void *arg)
{
    (void)arg;
    esp_restart();
}

static void on_end(uint16_t conn, const ns_board_v1_ControlRequest *req, ns_board_v1_ControlResponse *resp)
{
    (void)conn;
    (void)req;
    if (s_queue == NULL) {
        fail(resp, RC_INVALID_STATE, "no transfer");
        return;
    }
    // Chunks written before OtaEnd may still be queued.
    for (int waited = 0; s_done != s_queued && waited < END_DRAIN_MS; waited += 10) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    switch (ota_sm_end(&s_sm)) {
    case OTA_SM_END_NOT_ACTIVE:
        fail(resp, RC_INVALID_STATE, "no transfer");
        goto out;
    case OTA_SM_END_INCOMPLETE:
        fail(resp, RC_INVALID_STATE, "image incomplete");
        goto out;
    case OTA_SM_END_VERIFY:
        break;
    }
    uint8_t digest[OTA_SM_SHA256_LEN];
    size_t digest_len = 0;
    if (psa_hash_finish(&s_hash, digest, sizeof(digest), &digest_len) != PSA_SUCCESS ||
        digest_len != sizeof(digest) || !ota_sm_hash_matches(&s_sm, digest)) {
        discard("SHA-256 mismatch");
        fail(resp, RC_VERIFY_FAILED, "sha256 mismatch");
        goto out;
    }
    // Checks the image format, the chip and the image's own hash.
    esp_err_t err = esp_ota_end(s_handle);
    s_handle = 0;
    if (err == ESP_OK) {
        err = esp_ota_set_boot_partition(s_part);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "image rejected: %s", esp_err_to_name(err));
        discard("image rejected");
        fail(resp, RC_VERIFY_FAILED, esp_err_to_name(err));
        goto out;
    }
    // Restart after the response has gone out.
    s_restarting = true;
    static esp_timer_handle_t timer;
    const esp_timer_create_args_t args = {.callback = restart, .name = "ota_restart"};
    if (timer == NULL && esp_timer_create(&args, &timer) != ESP_OK) {
        timer = NULL;
    }
    if (timer == NULL || esp_timer_start_once(timer, RESTART_DELAY_US) != ESP_OK) {
        ESP_LOGE(TAG, "restart timer failed, restarting now");
        esp_restart();
    }
    resp->result = RC_OK;
    ESP_LOGI(TAG, "update verified, restarting into %s (OTA task stack left %u, min free heap %u)", s_part->label,
             (unsigned)uxTaskGetStackHighWaterMark(s_task), (unsigned)esp_get_minimum_free_heap_size());
out:
    xSemaphoreGive(s_lock);
}

static void on_abort(uint16_t conn, const ns_board_v1_ControlRequest *req, ns_board_v1_ControlResponse *resp)
{
    (void)conn;
    (void)req;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_restarting && (s_sm.active || s_in_ota_state)) {
        discard("OtaAbort");
    }
    resp->result = RC_OK;
    xSemaphoreGive(s_lock);
}

esp_err_t ota_init(ota_active_cb_t on_active)
{
    ota_confirm_init();
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    s_on_active = on_active;
    ota_sm_init(&s_sm, WINDOW_BYTES);
    link_set_command_handler(ns_board_v1_ControlRequest_ota_begin_tag, on_begin);
    link_set_command_handler(ns_board_v1_ControlRequest_ota_end_tag, on_end);
    link_set_command_handler(ns_board_v1_ControlRequest_ota_abort_tag, on_abort);
    link_set_ota_write_handler(on_write);
    link_set_hello_confirm_hook(ota_confirm_on_hello);
    return ESP_OK;
}
