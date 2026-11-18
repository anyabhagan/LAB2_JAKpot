#include <stdbool.h>
#include <stdint.h>

#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "board.h"

#if defined(Q3_EQUAL_PRIORITIES) && Q3_EQUAL_PRIORITIES
static const int ledOnPriority  = 1;
static const int ledOffPriority = 1;
static const int uartPriority   = 1;
#else
static const int ledOnPriority  = 3;
static const int ledOffPriority = 2;
static const int uartPriority   = 1;
#endif

static StaticSemaphore_t ledMutexStorage;
static SemaphoreHandle_t ledMutex;

typedef enum {
    READY_FOR_ON,
    READY_FOR_OFF,
    HOLDING_OFF
} LedPhase;

typedef struct {
    LedPhase phase;
    bool isOn;
    bool haveOnDuration;
    bool haveOffDuration;
    TickType_t changedAt;
    TickType_t onDuration;
    TickType_t offDuration;
    TickType_t onWaitFinishedAt;
    TickType_t offHandoff;
} LedStatus;

/* The mutex protects both the GPIO and this shared status. */
static LedStatus led = { .phase = READY_FOR_ON };

static void task_led_on(void *argument)
{
    (void)argument;

    for (;;) {
        bool completedOnInterval = false;

        if (xSemaphoreTake(ledMutex, portMAX_DELAY) == pdTRUE) {
            if (led.phase == READY_FOR_ON) {
                board_led_on();
                const TickType_t now = xTaskGetTickCount();

                if (led.haveOnDuration) {
                    led.offDuration = (TickType_t)(now - led.changedAt);
                    led.haveOffDuration = true;
                }

                led.isOn = true;
                led.changedAt = now;

                while ((TickType_t)(xTaskGetTickCount() - now)
                       < pdMS_TO_TICKS(500)) {
                    /* Actively wait for 500 ms. */
                }
                led.onWaitFinishedAt = xTaskGetTickCount();
                led.phase = READY_FOR_OFF;
                completedOnInterval = true;
            }

            xSemaphoreGive(ledMutex);
        }

        if (completedOnInterval) {
            taskYIELD();
        }

        /* Give lower-priority tasks time to run. */
        vTaskDelay(1);
    }
}

static void task_led_off(void *argument)
{
    (void)argument;

    for (;;) {
        bool turnedOff = false;

        if (xSemaphoreTake(ledMutex, portMAX_DELAY) == pdTRUE) {
            if (led.phase == READY_FOR_OFF) {
                board_led_off();
                const TickType_t now = xTaskGetTickCount();

                led.onDuration = (TickType_t)(now - led.changedAt);
                led.offHandoff = (TickType_t)(now - led.onWaitFinishedAt);
                led.haveOnDuration = true;
                led.isOn = false;
                led.changedAt = now;
                led.phase = HOLDING_OFF;
                turnedOff = true;
            }

            xSemaphoreGive(ledMutex);
        }

        if (turnedOff) {
            vTaskDelay(pdMS_TO_TICKS(1000));

            if (xSemaphoreTake(ledMutex, portMAX_DELAY) == pdTRUE) {
                led.phase = READY_FOR_ON;
                xSemaphoreGive(ledMutex);
            }
        } else {
            vTaskDelay(1);
        }
    }
}

static void uart_write_ms(TickType_t ticks)
{
    uint32_t value = (uint32_t)(((uint64_t)ticks * 1000U)
                                / configTICK_RATE_HZ);
    char digits[11];
    char *position = &digits[10];
    *position = '\0';

    do {
        *--position = (char)('0' + value % 10U);
        value /= 10U;
    } while (value != 0U);

    board_uart_write(position);
    board_uart_write(" ms");
}

static void task_uart(void *argument)
{
    (void)argument;
    #if defined(Q3_EQUAL_PRIORITIES) && Q3_EQUAL_PRIORITIES
    board_uart_write("Q3 | COOPERATIVE | EQUAL 1/1/1\r\n");
#else
    board_uart_write("Q3 | COOPERATIVE | OPTION A 3/2/1\r\n");
#endif

    for (;;) {
        LedStatus snapshot;

        if (xSemaphoreTake(ledMutex, portMAX_DELAY) == pdTRUE) {
            snapshot = led;
            xSemaphoreGive(ledMutex);

            board_uart_write(snapshot.isOn ? "LED ON" : "LED OFF");
            board_uart_write(" | last change: ");
            uart_write_ms(snapshot.changedAt);

            if (snapshot.haveOnDuration) {
               board_uart_write(" | last ON: ");
               uart_write_ms(snapshot.onDuration);
               board_uart_write(" | OFF handoff: ");
               uart_write_ms(snapshot.offHandoff);
         }

            board_uart_write("\r\n");
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

int main(void)
{
    board_init();

    ledMutex = xSemaphoreCreateMutexStatic(&ledMutexStorage);

    if (ledMutex == NULL) {
        board_uart_write("LED mutex creation failed\r\n");
        for (;;) {}
    }

    if (xTaskCreate(task_led_on, "LED_ON", 256, NULL,
                    ledOnPriority, NULL) != pdPASS ||
        xTaskCreate(task_led_off, "LED_OFF", 256, NULL,
                    ledOffPriority, NULL) != pdPASS ||
        xTaskCreate(task_uart, "UART", 256, NULL,
                    uartPriority, NULL) != pdPASS) {
        board_uart_write("Task creation failed\r\n");
        for (;;) {}
    }

    vTaskStartScheduler();

    board_uart_write("Scheduler failed to start\r\n");
    for (;;) {}
}