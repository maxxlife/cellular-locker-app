#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/net/http/server.h>
#include <zephyr/net/http/service.h>
#include <zephyr/net/socket.h> 
#include <zephyr/logging/log.h>
#include <modem/lte_lc.h>
#include <modem/nrf_modem_lib.h>
#include <modem/sms.h>
#include <stdio.h>
#include <zephyr/sys/reboot.h>

/* ================= SECURITY CONFIG ================= */
#define SMS_SECRET_KEY_WAKE "WK_1234"
#define SMS_SECRET_KEY_RESET "RST_1234"

#define CMD_WAKE  "WAKE:" SMS_SECRET_KEY_WAKE
#define CMD_RESET "RESET:" SMS_SECRET_KEY_RESET
/* =================================================== */

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

#define SW0_NODE DT_ALIAS(sw0)
#define SW1_NODE DT_ALIAS(sw1)
#define SW2_NODE DT_ALIAS(sw2)
#define SW3_NODE DT_ALIAS(sw3)

static const struct gpio_dt_spec buttons[COMPARTMENT_COUNT] = {
    GPIO_DT_SPEC_GET(SW0_NODE, gpios),
    GPIO_DT_SPEC_GET(SW1_NODE, gpios),
    GPIO_DT_SPEC_GET(SW2_NODE, gpios),
    GPIO_DT_SPEC_GET(SW3_NODE, gpios),
};

static struct gpio_callback button_cb_data[COMPARTMENT_COUNT];

struct compartment {
    int id;
    bool is_open;
};

static struct compartment compartments[COMPARTMENT_COUNT];

/* ================= SLEEP / WAKE LOGIC ================= */

static struct k_timer inactivity_timer;
static struct k_work_delayable button_check_work;
static struct k_work sleep_work;
static struct k_work wake_work; 
static struct k_work reset_work; 
static bool is_sleeping = false;

#define INACTIVITY_TIMEOUT K_SECONDS(60)
#define BUTTON_DEBOUNCE    K_MSEC(100)

static void reset_inactivity_timer(void)
{
    if (!is_sleeping) {
        k_timer_start(&inactivity_timer, INACTIVITY_TIMEOUT, K_NO_WAIT);
    }
}

static void restore_led_states(void)
{
    for (int i = 0; i < COMPARTMENT_COUNT; i++) {
        gpio_pin_set_dt(&leds[i], compartments[i].is_open ? 1 : 0);
    }
}

static void play_sleep_animation(void)
{
    LOG_INF("UX: Playing Sleep Animation");
    for (int i = 0; i < 2; i++) {
        for (int j = 0; j < COMPARTMENT_COUNT; j++) {
            gpio_pin_set_dt(&leds[j], 1);
        }
        k_sleep(K_MSEC(150));
        
        for (int j = 0; j < COMPARTMENT_COUNT; j++) {
            gpio_pin_set_dt(&leds[j], 0);
        }
        k_sleep(K_MSEC(150));
    }
    restore_led_states();
}

static void play_wake_animation(void)
{
    LOG_INF("UX: Playing Wake Animation");
    
    for (int i = 0; i < COMPARTMENT_COUNT; i++) {
        gpio_pin_set_dt(&leds[i], 1);
        k_sleep(K_MSEC(80));
        gpio_pin_set_dt(&leds[i], 0);
    }
    
    for (int i = 0; i < COMPARTMENT_COUNT; i++) {
        gpio_pin_set_dt(&leds[i], 1);
    }
    k_sleep(K_MSEC(200));
    
    restore_led_states();
}

static void play_reset_animation(void)
{
    LOG_INF("UX: Playing Reset (Reboot) Animation");
    
    for (int i = 0; i < 3; i++) {
        gpio_pin_set_dt(&leds[0], 1); gpio_pin_set_dt(&leds[2], 1);
        gpio_pin_set_dt(&leds[1], 0); gpio_pin_set_dt(&leds[3], 0);
        k_sleep(K_MSEC(100));
        
        gpio_pin_set_dt(&leds[0], 0); gpio_pin_set_dt(&leds[2], 0);
        gpio_pin_set_dt(&leds[1], 1); gpio_pin_set_dt(&leds[3], 1);
        k_sleep(K_MSEC(100));
    }
    
    for (int j = 0; j < COMPARTMENT_COUNT; j++) {
        gpio_pin_set_dt(&leds[j], 1);
    }
    k_sleep(K_MSEC(500)); 
}

static void wake_network(void)
{
    int sock = zsock_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock >= 0) {
        struct sockaddr_in addr = {
            .sin_family = AF_INET,
            .sin_port = htons(8080),
        };
        zsock_inet_pton(AF_INET, "8.8.8.8", &addr.sin_addr);
        zsock_sendto(sock, "WAKE", 4, 0, (struct sockaddr *)&addr, sizeof(addr));
        zsock_close(sock);
        LOG_INF("Network radio awakened via dummy UDP uplink packet");
    }
}

static void wake_work_handler(struct k_work *work)
{
    LOG_INF(">>> EXECUTING WAKEUP SEQUENCE <<<");
    play_wake_animation();
    wake_network();
    reset_inactivity_timer();
}

static void reset_work_handler(struct k_work *work)
{
    LOG_WRN(">>> EXECUTING SYSTEM REBOOT SEQUENCE <<<");
    play_reset_animation();
    sys_reboot(SYS_REBOOT_COLD);
}

static void trigger_wakeup(void)
{
    if (is_sleeping) {
        is_sleeping = false; 
        k_work_submit(&wake_work);
    }
}

static void sleep_work_handler(struct k_work *work)
{
    LOG_WRN("=========================================");
    LOG_WRN("   60s INACTIVITY REACHED");
    LOG_WRN("   Locker is now in APP SLEEP STATE");
    LOG_WRN("   Modem dropping to eDRX (10.24s cycle).");
    LOG_WRN("   Waiting for SMS, Button 0+1, or HTTP to wake...");
    LOG_WRN("=========================================");
    
    play_sleep_animation();
}

static void inactivity_timer_handler(struct k_timer *timer_id)
{
    is_sleeping = true;
    k_work_submit(&sleep_work);
}

/* ================= SMS & BUTTON HANDLERS ================= */

static void sms_callback(struct sms_data *const data, void *context)
{
    if (data == NULL) return;

    LOG_INF("=========================================");
    LOG_INF("   SMS RECEIVED!");
    LOG_INF("   Payload: %.*s", data->payload_len, data->payload);
    LOG_INF("=========================================");

    if (data->payload_len >= strlen(CMD_RESET) && 
        strncmp(data->payload, CMD_RESET, strlen(CMD_RESET)) == 0) {
        
        LOG_WRN(">>> AUTHORIZED RESET COMMAND DETECTED! <<<");
        k_work_submit(&reset_work); 
        return; 
    }

    if (data->payload_len >= strlen(CMD_WAKE) && 
        strncmp(data->payload, CMD_WAKE, strlen(CMD_WAKE)) == 0) {
        if (is_sleeping) {
            LOG_INF(">>> AUTHORIZED SMS WAKEUP DETECTED! <<<");
            trigger_wakeup();
        } else {
            LOG_INF("WAKE command received, but app is already awake.");
        }
        return;
    }

    LOG_WRN("Unauthorized or unrecognized SMS payload token. Ignoring.");
}

static void button_check_handler(struct k_work *work)
{
    if (is_sleeping) {
        int b0_pressed = gpio_pin_get_dt(&buttons[0]);
        int b1_pressed = gpio_pin_get_dt(&buttons[1]);

        if (b0_pressed && b1_pressed) {
            LOG_INF(">>> SIMULTANEOUS PRESS DETECTED! <<<");
            trigger_wakeup();
        }
    }
}

/* ================= GPIO LOGIC & KEYPAD FIX ================= */

static void close_compartment(int id);

// Internal helper logic separated from the HTTP API handler
static void do_open_compartment(int id)
{
    if (id < 0 || id >= COMPARTMENT_COUNT) return;
    gpio_pin_set_dt(&leds[id], 1);
    compartments[id].is_open = true;
}

static bool is_typing_pin = false;
static uint8_t pin_digits_entered = 0;
static uint8_t current_pin_buffer[4];  // Array to hold entered digits
static int last_pressed_button = -1;
static int64_t last_button_press_time = 0; // Prevent hardware bounce
static struct k_work_delayable keypad_timeout_work;

#define KEYPAD_TIMEOUT K_SECONDS(3)

static void keypad_timeout_handler(struct k_work *work)
{
    if (is_typing_pin) {
        // If only 1 digit was entered and timer expires, treat it as a deliberate CLOSE action
        if (pin_digits_entered == 1 && last_pressed_button != -1) {
            if (compartments[last_pressed_button].is_open) {
                close_compartment(last_pressed_button);
                LOG_INF("HARDWARE: Compartment %d physically CLOSED by user (Timeout)", last_pressed_button);
            }
        } else {
            LOG_INF("Keypad session timed out. Resetting state.");
        }
        
        is_typing_pin = false;
        pin_digits_entered = 0;
        last_pressed_button = -1;
    }
}

static void button_pressed(const struct device *dev,
                           struct gpio_callback *cb,
                           uint32_t pins)
{
    // Sleep combo bypasses debounce directly
    if (is_sleeping) {
        k_work_reschedule(&button_check_work, BUTTON_DEBOUNCE);
        return;
    }

    // HARDWARE DEBOUNCING: Ignore if press happened < 250ms ago
    int64_t now = k_uptime_get();
    if (now - last_button_press_time < 250) {
        return;
    }
    last_button_press_time = now;

    reset_inactivity_timer();

    for (int i = 0; i < COMPARTMENT_COUNT; i++) {
        if (pins & BIT(buttons[i].pin)) {
            
            // Track Active Keypad Sessions
            is_typing_pin = true;
            current_pin_buffer[pin_digits_entered] = i; // Store digit
            pin_digits_entered++;
            last_pressed_button = i;
            
            LOG_INF("Keypad input: Button %d (Digit %d/4)", i, pin_digits_entered);
            k_work_reschedule(&keypad_timeout_work, KEYPAD_TIMEOUT);

            if (pin_digits_entered == 4) {
                LOG_INF("4-digit PIN entry complete: %d-%d-%d-%d", 
                        current_pin_buffer[0], current_pin_buffer[1], 
                        current_pin_buffer[2], current_pin_buffer[3]);

                // VALIDATE PIN 1: Buttons 1-2-3-4 (Indexes 0,1,2,3) -> Opens Cell 1 (Comp 0)
                if (current_pin_buffer[0] == 0 && current_pin_buffer[1] == 1 && 
                    current_pin_buffer[2] == 2 && current_pin_buffer[3] == 3) {
                    LOG_INF(">>> Valid PIN! Opening Compartment 0 <<<");
                    do_open_compartment(0);
                }
                // VALIDATE PIN 2: Buttons 4-3-2-1 (Indexes 3,2,1,0) -> Opens Cell 2 (Comp 1)
                else if (current_pin_buffer[0] == 3 && current_pin_buffer[1] == 2 && 
                         current_pin_buffer[2] == 1 && current_pin_buffer[3] == 0) {
                    LOG_INF(">>> Valid PIN! Opening Compartment 1 <<<");
                    do_open_compartment(1);
                }
                else {
                    LOG_WRN(">>> Invalid PIN. Access Denied. <<<");
                }

                // Reset keypad state
                is_typing_pin = false;
                pin_digits_entered = 0;
                last_pressed_button = -1;
                k_work_cancel_delayable(&keypad_timeout_work);
                return; 
            }

            // Suppress the immediate door-close action while typing
            if (compartments[i].is_open) {
                LOG_INF("Suppressing immediate close for compartment %d (active PIN session)", i);
            }

            break; // Stop evaluating multiple pins to prevent multi-press errors
        }
    }
}

static void close_compartment(int id)
{
    if (id < 0 || id >= COMPARTMENT_COUNT) return;
    gpio_pin_set_dt(&leds[id], 0);
    compartments[id].is_open = false;
}

static void init_compartments(void)
{
    for (int i = 0; i < COMPARTMENT_COUNT; i++) {
        compartments[i].id = i;
        compartments[i].is_open = false;
    }
}

/* ================= HTTP ENDPOINTS ================= */

static int on_locker_status(struct http_client_ctx *client, enum http_data_status status,
                            const struct http_request_ctx *req, struct http_response_ctx *res, void *user_data)
{
    static char body[128];
    if (status != HTTP_SERVER_DATA_FINAL) return 0;

    if (is_sleeping) {
        LOG_INF(">>> WAKING UP VIA HTTP REQUEST (GET /locker/status) <<<");
        trigger_wakeup();
    }

    reset_inactivity_timer();
    LOG_INF("API REQUEST: GET /locker/status");

    int len = snprintf(body, sizeof(body),
        "{\"locker_id\":\"locker-demo-001\",\"status\":\"online\",\"compartments\":%d}", COMPARTMENT_COUNT);

    res->status = HTTP_200_OK; res->body = (uint8_t *)body; res->body_len = len; res->final_chunk = true;
    return 0;
}

static int on_compartments_list(struct http_client_ctx *client, enum http_data_status status,
                                const struct http_request_ctx *req, struct http_response_ctx *res, void *user_data)
{
    static char body[256];
    if (status != HTTP_SERVER_DATA_FINAL) return 0;

    if (is_sleeping) {
        LOG_INF(">>> WAKING UP VIA HTTP REQUEST (GET /compartments) <<<");
        trigger_wakeup();
    }

    reset_inactivity_timer();
    LOG_INF("API REQUEST: GET /compartments");

    int offset = 0;
    offset += snprintf(body + offset, sizeof(body) - offset, "[");
    for (int i = 0; i < COMPARTMENT_COUNT; i++) {
        offset += snprintf(body + offset, sizeof(body) - offset, "{\"id\":%d,\"state\":\"%s\"}%s",
                           i, compartments[i].is_open ? "open" : "closed", (i < COMPARTMENT_COUNT - 1) ? "," : "");
    }
    offset += snprintf(body + offset, sizeof(body) - offset, "]");

    res->status = HTTP_200_OK; res->body = (uint8_t *)body; res->body_len = offset; res->final_chunk = true;
    return 0;
}

static int open_compartment(int id, struct http_response_ctx *res)
{
    static char body[64];
    
    if (is_sleeping) {
        LOG_INF(">>> WAKING UP VIA HTTP REQUEST (POST /compartments/%d/open) <<<", id);
        trigger_wakeup();
    }

    reset_inactivity_timer();
    LOG_INF("API REQUEST: POST /compartments/%d/open", id);

    if (id < 0 || id >= COMPARTMENT_COUNT) {
        res->status = HTTP_400_BAD_REQUEST; return 0;
    }

    do_open_compartment(id);

    int len = snprintf(body, sizeof(body), "{\"status\":\"opened\",\"id\":%d}", id);
    res->status = HTTP_200_OK; res->body = (uint8_t *)body; res->body_len = len; res->final_chunk = true;
    return 0;
}

static int close_compartment_api(int id, struct http_response_ctx *res)
{
    static char body[64];
    
    if (is_sleeping) {
        LOG_INF(">>> WAKING UP VIA HTTP REQUEST (POST /compartments/%d/close) <<<", id);
        trigger_wakeup();
    }

    reset_inactivity_timer();
    LOG_INF("API REQUEST: POST /compartments/%d/close", id);

    if (id < 0 || id >= COMPARTMENT_COUNT) {
        res->status = HTTP_400_BAD_REQUEST; return 0;
    }

    close_compartment(id);
    int len = snprintf(body, sizeof(body), "{\"status\":\"closed\",\"id\":%d}", id);
    res->status = HTTP_200_OK; res->body = (uint8_t *)body; res->body_len = len; res->final_chunk = true;
    return 0;
}

/* ================= CALLBACKS & SERVER DEF ================= */

#define OPEN_CB(ID) \
static int open##ID##_cb(struct http_client_ctx *client, enum http_data_status status, \
                         const struct http_request_ctx *req, struct http_response_ctx *res, void *user_data) \
{ if (status != HTTP_SERVER_DATA_FINAL) return 0; return open_compartment(ID, res); }

#define CLOSE_CB(ID) \
static int close##ID##_cb(struct http_client_ctx *client, enum http_data_status status, \
                          const struct http_request_ctx *req, struct http_response_ctx *res, void *user_data) \
{ if (status != HTTP_SERVER_DATA_FINAL) return 0; return close_compartment_api(ID, res); }

OPEN_CB(0) OPEN_CB(1) OPEN_CB(2) OPEN_CB(3)
CLOSE_CB(0) CLOSE_CB(1) CLOSE_CB(2) CLOSE_CB(3)

static uint16_t service_port = 8080;
HTTP_SERVICE_DEFINE(locker_svc, "0.0.0.0", &service_port, 2, 10, NULL, NULL, NULL);

static struct http_resource_detail_dynamic status_detail = {
    .common = { .type = HTTP_RESOURCE_TYPE_DYNAMIC, .bitmask_of_supported_http_methods = BIT(HTTP_GET), }, .cb = on_locker_status,
};
HTTP_RESOURCE_DEFINE(status_resource, locker_svc, "/locker/status", &status_detail);

static struct http_resource_detail_dynamic list_detail = {
    .common = { .type = HTTP_RESOURCE_TYPE_DYNAMIC, .bitmask_of_supported_http_methods = BIT(HTTP_GET), }, .cb = on_compartments_list,
};
HTTP_RESOURCE_DEFINE(list_resource, locker_svc, "/compartments", &list_detail);

#define OPEN_RESOURCE(ID) \
static struct http_resource_detail_dynamic open##ID##_detail = { \
    .common = { .type = HTTP_RESOURCE_TYPE_DYNAMIC, .bitmask_of_supported_http_methods = BIT(HTTP_POST), }, .cb = open##ID##_cb, \
}; HTTP_RESOURCE_DEFINE(open##ID##_resource, locker_svc, "/compartments/"#ID"/open", &open##ID##_detail);
OPEN_RESOURCE(0) OPEN_RESOURCE(1) OPEN_RESOURCE(2) OPEN_RESOURCE(3)

#define CLOSE_RESOURCE(ID) \
static struct http_resource_detail_dynamic close##ID##_detail = { \
    .common = { .type = HTTP_RESOURCE_TYPE_DYNAMIC, .bitmask_of_supported_http_methods = BIT(HTTP_POST), }, .cb = close##ID##_cb, \
}; HTTP_RESOURCE_DEFINE(close##ID##_resource, locker_svc, "/compartments/"#ID"/close", &close##ID##_detail);
CLOSE_RESOURCE(0) CLOSE_RESOURCE(1) CLOSE_RESOURCE(2) CLOSE_RESOURCE(3)

/* ================= LTE ================= */

static void lte_handler(const struct lte_lc_evt *evt)
{
    switch (evt->type) {
    case LTE_LC_EVT_NW_REG_STATUS:
        if (evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_HOME ||
            evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_ROAMING) {
            LOG_INF("--- LTE Network Attached! ---");
            k_sem_give(&lte_connected);
        }
        break;
    case LTE_LC_EVT_RRC_UPDATE:
        LOG_INF("--- RRC Mode changed: %s ---", 
                evt->rrc_mode == LTE_LC_RRC_MODE_CONNECTED ? "CONNECTED" : "IDLE (Power Saving)");
        break;
    default:
        break;
    }
}

/* ================= MAIN ================= */

int main(void)
{
    int err;

    LOG_INF("Cellular Locker Demo - Step 4 (eDRX + SMS Wake + LED UX)");
    
    k_timer_init(&inactivity_timer, inactivity_timer_handler, NULL);
    k_work_init_delayable(&button_check_work, button_check_handler);
    k_work_init(&sleep_work, sleep_work_handler);
    k_work_init(&wake_work, wake_work_handler);
    k_work_init(&reset_work, reset_work_handler);

    // Initialize the new keypad timeout logic
    k_work_init_delayable(&keypad_timeout_work, keypad_timeout_handler);

    for (int i = 0; i < COMPARTMENT_COUNT; i++) {
        if (!device_is_ready(leds[i].port)) continue;
        gpio_pin_configure_dt(&leds[i], GPIO_OUTPUT_INACTIVE);
    }

    for (int i = 0; i < COMPARTMENT_COUNT; i++) {
        if (!device_is_ready(buttons[i].port)) continue;
        gpio_pin_configure_dt(&buttons[i], GPIO_INPUT);
        gpio_pin_interrupt_configure_dt(&buttons[i], GPIO_INT_EDGE_TO_ACTIVE);
        gpio_init_callback(&button_cb_data[i], button_pressed, BIT(buttons[i].pin));
        gpio_add_callback(buttons[i].port, &button_cb_data[i]);
    }

    init_compartments();

    err = nrf_modem_lib_init();
    if (err) return err;

    // Register SMS Listener
    err = sms_register_listener(sms_callback, NULL);
    if (err) LOG_ERR("Failed to register SMS listener: %d", err);

    /* ---------------------------------------------------------*/

    err = lte_lc_connect_async(lte_handler);
    if (err) return err;

    k_sem_take(&lte_connected, K_FOREVER);
    LOG_INF("LTE Connected for the first time");

    err = http_server_start();
    if (err) {
        LOG_ERR("HTTP server failed");
    } else {
        LOG_INF("HTTP server started on port %d", service_port);
    }

    is_sleeping = false;
    reset_inactivity_timer();
    LOG_INF("App logic awake. 60s inactivity timer started.");

    while (1) {
        k_sleep(K_SECONDS(1));
    }
}