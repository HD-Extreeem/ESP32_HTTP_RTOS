
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "nvs_flash.h"
#include "driver/gpio.h"

#include "esp_wifi.h"
#include "esp_event_loop.h"
#include "esp_log.h"
#include "esp_system.h"

#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "lwip/netdb.h"
#include "lwip/dns.h"

#include <time.h>

//Onboard led pin
#define BLINK_GPIO 5

//Button and laser pins
#define laser 4
#define btn 13

//Ultrasonic pins
#define echo 17
#define trig 16

#define wifi_ssid "ssid_here"
#define wifi_pass "pass_here"

static const char *TAG_WIFI="TEST_WIFI";
static const char *TAG_HTTP="HTTP_TASK";

static EventGroupHandle_t s_wifi_event_group;

const int IPV4_GOTIP_BIT = BIT0;

#define WEB_SERVER "maker.ifttt.com"
#define WEB_PORT 80
#define WEB_URL "web_url_here"

//Request GET-header when sending
static const char *REQUEST = "GET " WEB_URL " HTTP/1.0\r\n"
    "Host: "WEB_SERVER"\r\n"
    "User-Agent: esp-idf/1.0 esp32\r\n"
    "\r\n";

// Task that prints hello to the serial output
// Also send a HTTP request after 10th time the task is run
void hello_task(void *pvParameter)
{
    uint16_t i = 0;
    while(1){
        ++i;
        printf("Hello! %d\n",i);
        vTaskDelay(1000 / portTICK_RATE_MS);
    }
}

// Task that toggles the led avaiable onboard the development board
void blink_task(void* ignore)
{
    gpio_pad_select_gpio(BLINK_GPIO);
    /* Set the GPIO as a push/pull output */
    gpio_set_direction(BLINK_GPIO, GPIO_MODE_OUTPUT);
    uint8_t status = 1;

    while(1) {
        /* Blink off (output low) */
        gpio_set_level(BLINK_GPIO, status);
        status = (status == 1) ? 0:1;
        vTaskDelay(1000 / portTICK_RATE_MS);
    }
}

//Task that reads the button state whether the buttons have been pushed or not
//If it is pushed then the activate the laser else deactivate
void button_task(void* ignore)
{
    //gpio_pad_select_gpio(laser);
    //gpio_pad_select_gpio(btn);
    /* Set the GPIO as a push/pull output */
    gpio_set_direction(laser, GPIO_MODE_OUTPUT);
    gpio_set_direction(btn, GPIO_MODE_INPUT);
    while(1) {
        /* Blink off (output low) */
        if(gpio_get_level(btn)){
            gpio_set_level(laser, 1);
        } else{
            gpio_set_level(laser, 0);
        }
        vTaskDelay(100 / portTICK_RATE_MS);
    }
}

//Task for measuring the distance with Ultrasonic
//Still under development...
void ultrasonic_task(void* ignore){
    //long duration;
    double distance = 0;
    uint32_t time_before = 0, time_after = 0;
    gpio_set_direction(echo, GPIO_MODE_INPUT);
    gpio_set_direction(trig, GPIO_MODE_OUTPUT);

    while(1){
        gpio_set_level(trig, 0);
        vTaskDelay(2 / portTICK_RATE_MS);

        gpio_set_level(trig, 1);
        vTaskDelay(0.01 / portTICK_RATE_MS);
        gpio_set_level(trig, 0);

        time_before = (uint32_t) (clock() * 1000 / CLOCKS_PER_SEC);
        //printf("T before = %d",time_before);
        while(gpio_get_level(echo) != 1);

        time_after = (uint32_t) (clock() * 1000 / CLOCKS_PER_SEC);
        //printf("T after = %d",time_after);
        distance = (((time_after-time_before)/2) / 29.1);
        printf("Distance = %lf, bef = %d/aft= %d \n",distance,time_before,time_after);

        vTaskDelay(200 / portTICK_RATE_MS);
    }
}

//Task for http post request
static void http_post_task(void *pvParameters)
{
    const struct addrinfo hints = {
        .ai_family = AF_INET,
        .ai_socktype = SOCK_STREAM,
    };
    struct addrinfo *res;
    struct in_addr *addr;
    int s, r;
    char recv_buf[64];

    while(1) {
        int err = getaddrinfo(WEB_SERVER, "80", &hints, &res);

        if(err != 0 || res == NULL) {
            ESP_LOGE(TAG_HTTP, "DNS lookup failed err=%d res=%p", err, res);
            vTaskDelay(1000 / portTICK_PERIOD_MS);
            continue;
        }

        addr = &((struct sockaddr_in *)res->ai_addr)->sin_addr;
        ESP_LOGI(TAG_HTTP, "DNS lookup succeeded. IP=%s", inet_ntoa(*addr));

        s = socket(res->ai_family, res->ai_socktype, 0);

        if(s < 0) {
            ESP_LOGE(TAG_HTTP, "... Failed to allocate socket.");
            freeaddrinfo(res);
            vTaskDelay(5000 / portTICK_PERIOD_MS);
            continue;
        }

        ESP_LOGI(TAG_HTTP, "... allocated socket");

        //Connect to the host adress
        if(connect(s, res->ai_addr, res->ai_addrlen) != 0) {
            ESP_LOGE(TAG_HTTP, "... socket connect failed errno=%d", errno);
            close(s);
            freeaddrinfo(res);
            vTaskDelay(5000 / portTICK_PERIOD_MS);
            continue;
        }

        ESP_LOGI(TAG_HTTP, "Connected");
        freeaddrinfo(res);

        //Write the data to the host adress that is connected to
        if (write(s, REQUEST, strlen(REQUEST)) < 0) {
            ESP_LOGE(TAG_HTTP, "... socket send failed");
            close(s);
            vTaskDelay(5000 / portTICK_PERIOD_MS);
            continue;
        }

        ESP_LOGI(TAG_HTTP, "... socket send success");

        struct timeval receiving_timeout;
        receiving_timeout.tv_sec = 5;
        receiving_timeout.tv_usec = 0;

        if (setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &receiving_timeout,
                sizeof(receiving_timeout)) < 0) {
            ESP_LOGE(TAG_HTTP, "... failed to set socket receiving timeout");
            close(s);
            vTaskDelay(5000 / portTICK_PERIOD_MS);
            continue;
        }

        ESP_LOGI(TAG_HTTP, "... set socket receiving timeout success");

        // Read HTTP post response and store the data in a buffert
        do {
            bzero(recv_buf, sizeof(recv_buf));
            r = read(s, recv_buf, sizeof(recv_buf)-1);
            for(int i = 0; i < r; i++) {
                putchar(recv_buf[i]);
            }
        } while(r > 0);

        ESP_LOGI(TAG_HTTP, "... done reading from socket. Last read return=%d errno=%d\r\n", r, errno);
        close(s);

        for(int countdown = 10; countdown >= 0; countdown--) {
            ESP_LOGI(TAG_HTTP, "%d... ", countdown);
            vTaskDelay(1000 / portTICK_PERIOD_MS);
        }
        ESP_LOGI(TAG_HTTP, "Sending again!");
    }
}

static esp_err_t event_handler(void *ctx, system_event_t *event)
{
    switch(event->event_id) {
    case SYSTEM_EVENT_STA_START:
        ESP_LOGI(TAG_WIFI, "SYSTEM_EVENT_STA_START");
        ESP_ERROR_CHECK(esp_wifi_connect());
        break;

    case SYSTEM_EVENT_STA_GOT_IP:
        xEventGroupSetBits(s_wifi_event_group, IPV4_GOTIP_BIT);
        ESP_LOGI(TAG_WIFI, "SYSTEM_EVENT_STA_GOT_IP");
        ESP_LOGI(TAG_WIFI, "Got IP: '%s'",
                 ip4addr_ntoa(&event->event_info.got_ip.ip_info.ip));
        break;

    case SYSTEM_EVENT_STA_DISCONNECTED:
        ESP_LOGI(TAG_WIFI, "SYSTEM_EVENT_STA_DISCONNECTED");
        xEventGroupClearBits(s_wifi_event_group, IPV4_GOTIP_BIT);
        ESP_ERROR_CHECK(esp_wifi_connect());
        break;

    default:
        break;
    }
    return ESP_OK;
}

static void initialise_wifi(void)
{
    tcpip_adapter_init();
    s_wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_event_loop_init(event_handler, NULL) );

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = wifi_ssid,
            .password = wifi_pass,
        },
    };

    ESP_LOGI(TAG_WIFI, "Setting WiFi configuration SSID %s...", wifi_config.sta.ssid);
    ESP_ERROR_CHECK( esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK( esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config) );
    ESP_ERROR_CHECK( esp_wifi_start() );

    ESP_LOGI(TAG_WIFI, "wifi_init_sta finished.");
    ESP_LOGI(TAG_WIFI, "connect to ap SSID:%s password:%s", wifi_ssid, wifi_pass);
}

void app_main()
{
    nvs_flash_init();
    initialise_wifi();

    xTaskCreate(&hello_task, "hello_task", 1024, NULL, 3, NULL);
    //xTaskCreate(&ultrasonic_task, "hello_task", 2048, NULL, 5, NULL);

    xTaskCreate(&blink_task, "blink_task", 1024, NULL, 1, NULL);
    //xTaskCreate(&button_task,"btn_task",2048,NULL,2,NULL);
    xEventGroupWaitBits(s_wifi_event_group, IPV4_GOTIP_BIT, false, true, portMAX_DELAY);
    ESP_LOGI(TAG_HTTP, "Connected and got IP!");
    xTaskCreate(&http_post_task, "http_req_task", 4096, NULL, 5, NULL);
}