#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/http/server.h>
#include <zephyr/net/http/service.h>
#include <zephyr/logging/log.h>
#include <modem/lte_lc.h>
#include <modem/nrf_modem_lib.h>
#include <stdio.h>
#include <stdlib.h>

LOG_MODULE_REGISTER(locker_app, LOG_LEVEL_DBG);

static K_SEM_DEFINE(lte_connected, 0, 1);

/* Work items for non-blocking GPIO operations */
static struct k_work open_compartment_work;
static struct k_work_delayable close_compartment_work;

#define PORT 80
#define COMPARTMENT_COUNT 6

/* The devicetree node identifier for the "led0" alias. */
#define LED0_NODE DT_ALIAS(led0)

static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED0_NODE, gpios);

struct compartment {
    int id;
    bool is_open;
};

static struct compartment compartments[COMPARTMENT_COUNT];

/* Handler to turn the LED off */
static void close_compartment_handler(struct k_work *item)
{
    if (device_is_ready(led.port)) {
        gpio_pin_set_dt(&led, 0);
    }
}

/* Handler to turn the LED on and schedule it to turn off */
static void open_compartment_handler(struct k_work *item)
{
    if (device_is_ready(led.port)) {
        gpio_pin_set_dt(&led, 1);
        /* Schedule the close handler to run 500ms from now */
        k_work_schedule(&close_compartment_work, K_MSEC(500));
    }
}

static void init_compartments(void)
{
    for (int i = 0; i < COMPARTMENT_COUNT; i++) {
        compartments[i].id = i;
        compartments[i].is_open = false;
    }
}

static int on_locker_status(struct http_client_ctx *client,
                            enum http_data_status status,
                            const struct http_request_ctx *request_ctx,
                            struct http_response_ctx *response_ctx,
                            void *user_data)
{
    static char response_body[128];

    if (status == HTTP_SERVER_DATA_FINAL) {
        int len = snprintf(response_body, sizeof(response_body),
                           "{\n\"locker_id\": \"locker-demo-001\",\n\"status\": \"online\",\n\"compartments\": %d\n}",
                           COMPARTMENT_COUNT);

        response_ctx->status = HTTP_200_OK;
        response_ctx->body = (uint8_t *)response_body;
        response_ctx->body_len = len;
        response_ctx->final_chunk = true;
    }
    return 0;
}
static int on_compartments_list(struct http_client_ctx *client,
                                enum http_data_status status,
                                const struct http_request_ctx *request_ctx,
                                struct http_response_ctx *response_ctx,
                                void *user_data)
{
    static char response_body[512];

    if (status != HTTP_SERVER_DATA_FINAL) {
        return 0;
    }

    int offset = 0;

    offset += snprintf(response_body + offset,
                       sizeof(response_body) - offset,
                       "[");

    for (int i = 0; i < COMPARTMENT_COUNT; i++) {

        offset += snprintf(response_body + offset,
                           sizeof(response_body) - offset,
                           "{\"id\":%d,\"state\":\"%s\"}%s",
                           compartments[i].id,
                           compartments[i].is_open ? "open" : "closed",
                           (i < COMPARTMENT_COUNT - 1) ? "," : "");
    }

    offset += snprintf(response_body + offset,
                       sizeof(response_body) - offset,
                       "]");

    response_ctx->status = HTTP_200_OK;
    response_ctx->headers = "Content-Type: application/json\r\n";
    response_ctx->body = (uint8_t *)response_body;
    response_ctx->body_len = offset;
    response_ctx->final_chunk = true;

    return 0;
}
static int on_compartment_open(struct http_client_ctx *client,
                               enum http_data_status status,
                               const struct http_request_ctx *request_ctx,
                               struct http_response_ctx *response_ctx,
                               void *user_data)
{
    static char response_body[] = "{\"status\":\"success\"}";

    if (status != HTTP_SERVER_DATA_FINAL) {
        return 0;
    }

    int id = 0;  /* currently fixed */

    if (id >= COMPARTMENT_COUNT) {
        response_ctx->status = HTTP_400_BAD_REQUEST;
        return 0;
    }

    compartments[id].is_open = true;

    k_work_submit(&open_compartment_work);

    response_ctx->status = HTTP_200_OK;
    response_ctx->headers = "Content-Type: application/json\r\n";
    response_ctx->body = (uint8_t *)response_body;
    response_ctx->body_len = sizeof(response_body) - 1;
    response_ctx->final_chunk = true;

    return 0;
}

/* Resource Definitions */

static uint16_t service_port = 8080;

HTTP_SERVICE_DEFINE(locker_svc, "0.0.0.0", &service_port, 2, 10, NULL, NULL, NULL);

static struct http_resource_detail_dynamic status_resource_detail = {
    .common = {
        .type = HTTP_RESOURCE_TYPE_DYNAMIC,
        .bitmask_of_supported_http_methods = BIT(HTTP_GET),
    },
    .cb = on_locker_status,
};

HTTP_RESOURCE_DEFINE(status_resource,
                     locker_svc,
                     "/locker/status",
                     &status_resource_detail);


static struct http_resource_detail_dynamic list_resource_detail = {
    .common = {
        .type = HTTP_RESOURCE_TYPE_DYNAMIC,
        .bitmask_of_supported_http_methods = BIT(HTTP_GET)
    },
    .cb = on_compartments_list,
};

HTTP_RESOURCE_DEFINE(list_resource,
                     locker_svc,
                     "/compartments",
                     &list_resource_detail);

static struct http_resource_detail_dynamic open_resource_detail = {
    .common = {
        .type = HTTP_RESOURCE_TYPE_DYNAMIC,
        .bitmask_of_supported_http_methods = BIT(HTTP_POST)
    },
    .cb = on_compartment_open,
};

HTTP_RESOURCE_DEFINE(open_resource,
                     locker_svc,
                     "/compartments/0/open",
                     &open_resource_detail);

static void lte_handler(const struct lte_lc_evt *const evt)
{
    if (evt->type == LTE_LC_EVT_NW_REG_STATUS) {
        if ((evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_HOME) ||
            (evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_ROAMING)) {
            LOG_INF("Network attached!");
            k_sem_give(&lte_connected);
        }
    }
}

int main(void)
{
    int err;

    LOG_INF("Starting Cellular Locker Demo on nRF9151-DK");

    if (!device_is_ready(led.port)) {
        LOG_ERR("LED device not ready");
    } else {
        gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);
    }

    init_compartments();

    k_work_init(&open_compartment_work, open_compartment_handler);
    k_work_init_delayable(&close_compartment_work, close_compartment_handler);

    err = nrf_modem_lib_init();
    if (err) {
        LOG_ERR("Modem library init failed: %d", err);
        return err;
    }

    LOG_INF("Connecting to LTE network...");
    err = lte_lc_connect_async(lte_handler);
    if (err) {
        LOG_ERR("Failed to initiate LTE connection: %d", err);
        return err;
    }
    
    k_sem_take(&lte_connected, K_FOREVER);
    LOG_INF("LTE Connected!");
    
    err = http_server_start();
    if (err) {
        LOG_ERR("Failed to start HTTP server (%d)", err);
    } else {
        LOG_INF("HTTP server started on port %d", service_port);
    }

    while (1) {
        k_sleep(K_MSEC(1000));
    }
    return 0;
}