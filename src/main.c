#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/net/http/server.h>
#include <zephyr/net/http/service.h>
#include <zephyr/logging/log.h>
#include <modem/lte_lc.h>
#include <modem/nrf_modem_lib.h>
#include <stdio.h>

LOG_MODULE_REGISTER(locker_app, LOG_LEVEL_INF);

static K_SEM_DEFINE(lte_connected, 0, 1);

#define COMPARTMENT_COUNT 4

#define LED0_NODE DT_ALIAS(led0)
#define LED1_NODE DT_ALIAS(led1)
#define LED2_NODE DT_ALIAS(led2)
#define LED3_NODE DT_ALIAS(led3)

static const struct gpio_dt_spec leds[COMPARTMENT_COUNT] = {
    GPIO_DT_SPEC_GET(LED0_NODE, gpios),
    GPIO_DT_SPEC_GET(LED1_NODE, gpios),
    GPIO_DT_SPEC_GET(LED2_NODE, gpios),
    GPIO_DT_SPEC_GET(LED3_NODE, gpios),
};

struct compartment {
    int id;
    bool is_open;
};

static struct compartment compartments[COMPARTMENT_COUNT];

static struct k_work open_compartment_work;
static struct k_work_delayable close_compartment_work;

static int active_compartment = -1;

/* ================= GPIO WORK ================= */

static void close_compartment_handler(struct k_work *item)
{
    if (active_compartment < 0 || active_compartment >= COMPARTMENT_COUNT)
        return;

    gpio_pin_set_dt(&leds[active_compartment], 0);
    compartments[active_compartment].is_open = false;
}

static void open_compartment_handler(struct k_work *item)
{
    if (active_compartment < 0 || active_compartment >= COMPARTMENT_COUNT)
        return;

    gpio_pin_set_dt(&leds[active_compartment], 1);
    k_work_schedule(&close_compartment_work, K_SECONDS(2));
}

/* ================= INIT ================= */

static void init_compartments(void)
{
    for (int i = 0; i < COMPARTMENT_COUNT; i++) {
        compartments[i].id = i;
        compartments[i].is_open = false;
    }
}

/* ================= STATUS ================= */

static int on_locker_status(struct http_client_ctx *client,
                            enum http_data_status status,
                            const struct http_request_ctx *req,
                            struct http_response_ctx *res,
                            void *user_data)
{
    static char body[128];

    if (status != HTTP_SERVER_DATA_FINAL)
        return 0;

    int len = snprintf(body, sizeof(body),
        "{\"locker_id\":\"locker-demo-001\",\"status\":\"online\",\"compartments\":%d}",
        COMPARTMENT_COUNT);

    res->status = HTTP_200_OK;
    res->body = (uint8_t *)body;
    res->body_len = len;
    res->final_chunk = true;

    return 0;
}

/* ================= LIST ================= */

static int on_compartments_list(struct http_client_ctx *client,
                                enum http_data_status status,
                                const struct http_request_ctx *req,
                                struct http_response_ctx *res,
                                void *user_data)
{
    static char body[256];

    if (status != HTTP_SERVER_DATA_FINAL)
        return 0;

    int offset = 0;

    offset += snprintf(body + offset, sizeof(body) - offset, "[");

    for (int i = 0; i < COMPARTMENT_COUNT; i++) {

        offset += snprintf(body + offset,
                           sizeof(body) - offset,
                           "{\"id\":%d,\"state\":\"%s\"}%s",
                           i,
                           compartments[i].is_open ? "open" : "closed",
                           (i < COMPARTMENT_COUNT - 1) ? "," : "");
    }

    offset += snprintf(body + offset, sizeof(body) - offset, "]");

    res->status = HTTP_200_OK;
    res->body = (uint8_t *)body;
    res->body_len = offset;
    res->final_chunk = true;

    return 0;
}

/* ================= OPEN ================= */

static int open_compartment(int id,
                            struct http_response_ctx *res)
{
    static char body[64];

    if (id < 0 || id >= COMPARTMENT_COUNT) {
        res->status = HTTP_400_BAD_REQUEST;
        return 0;
    }

    active_compartment = id;
    compartments[id].is_open = true;

    k_work_submit(&open_compartment_work);

    int len = snprintf(body, sizeof(body),
        "{\"status\":\"opened\",\"id\":%d}", id);

    res->status = HTTP_200_OK;
    res->body = (uint8_t *)body;
    res->body_len = len;
    res->final_chunk = true;

    return 0;
}

static int open0_cb(struct http_client_ctx *client,
                    enum http_data_status status,
                    const struct http_request_ctx *req,
                    struct http_response_ctx *res,
                    void *user_data)
{
    if (status != HTTP_SERVER_DATA_FINAL) return 0;
    return open_compartment(0, res);
}

static int open1_cb(struct http_client_ctx *client,
                    enum http_data_status status,
                    const struct http_request_ctx *req,
                    struct http_response_ctx *res,
                    void *user_data)
{
    if (status != HTTP_SERVER_DATA_FINAL) return 0;
    return open_compartment(1, res);
}

static int open2_cb(struct http_client_ctx *client,
                    enum http_data_status status,
                    const struct http_request_ctx *req,
                    struct http_response_ctx *res,
                    void *user_data)
{
    if (status != HTTP_SERVER_DATA_FINAL) return 0;
    return open_compartment(2, res);
}

static int open3_cb(struct http_client_ctx *client,
                    enum http_data_status status,
                    const struct http_request_ctx *req,
                    struct http_response_ctx *res,
                    void *user_data)
{
    if (status != HTTP_SERVER_DATA_FINAL) return 0;
    return open_compartment(3, res);
}

/* ================= HTTP SERVER ================= */

static uint16_t service_port = 8080;

HTTP_SERVICE_DEFINE(locker_svc,
                    "0.0.0.0",
                    &service_port,
                    2,
                    10,
                    NULL,
                    NULL,
                    NULL);

static struct http_resource_detail_dynamic status_detail = {
    .common = {
        .type = HTTP_RESOURCE_TYPE_DYNAMIC,
        .bitmask_of_supported_http_methods = BIT(HTTP_GET),
    },
    .cb = on_locker_status,
};

HTTP_RESOURCE_DEFINE(status_resource,
                     locker_svc,
                     "/locker/status",
                     &status_detail);

static struct http_resource_detail_dynamic list_detail = {
    .common = {
        .type = HTTP_RESOURCE_TYPE_DYNAMIC,
        .bitmask_of_supported_http_methods = BIT(HTTP_GET),
    },
    .cb = on_compartments_list,
};

HTTP_RESOURCE_DEFINE(list_resource,
                     locker_svc,
                     "/compartments",
                     &list_detail);

/* OPEN ENDPOINTS */

#define OPEN_RESOURCE(ID) \
static struct http_resource_detail_dynamic open##ID##_detail = { \
    .common = { \
        .type = HTTP_RESOURCE_TYPE_DYNAMIC, \
        .bitmask_of_supported_http_methods = BIT(HTTP_POST), \
    }, \
    .cb = open##ID##_cb, \
}; \
HTTP_RESOURCE_DEFINE(open##ID##_resource, \
                     locker_svc, \
                     "/compartments/"#ID"/open", \
                     &open##ID##_detail);

OPEN_RESOURCE(0)
OPEN_RESOURCE(1)
OPEN_RESOURCE(2)
OPEN_RESOURCE(3)

/* ================= LTE ================= */

static void lte_handler(const struct lte_lc_evt *evt)
{
    if (evt->type == LTE_LC_EVT_NW_REG_STATUS) {

        if (evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_HOME ||
            evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_ROAMING) {

            LOG_INF("Network attached");
            k_sem_give(&lte_connected);
        }
    }
}

/* ================= MAIN ================= */

int main(void)
{
    int err;

    LOG_INF("Cellular Locker Demo");

    for (int i = 0; i < COMPARTMENT_COUNT; i++) {

        if (!device_is_ready(leds[i].port)) {
            LOG_ERR("LED %d not ready", i);
            continue;
        }

        gpio_pin_configure_dt(&leds[i], GPIO_OUTPUT_INACTIVE);
    }

    init_compartments();

    k_work_init(&open_compartment_work, open_compartment_handler);
    k_work_init_delayable(&close_compartment_work, close_compartment_handler);

    err = nrf_modem_lib_init();
    if (err) {
        LOG_ERR("Modem init failed");
        return err;
    }

    LOG_INF("Connecting LTE");

    err = lte_lc_connect_async(lte_handler);
    if (err)
        return err;

    k_sem_take(&lte_connected, K_FOREVER);

    LOG_INF("LTE Connected");

    err = http_server_start();

    if (err)
        LOG_ERR("HTTP server failed");
    else
        LOG_INF("HTTP server started on port %d", service_port);

    while (1)
        k_sleep(K_SECONDS(1));
}