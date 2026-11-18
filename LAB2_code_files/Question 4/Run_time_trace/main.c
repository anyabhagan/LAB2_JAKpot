#include <stdbool.h>
#include <stdint.h>

#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "board.h"

#ifndef TRACE_EQUAL_PRIORITIES
#define TRACE_EQUAL_PRIORITIES 0
#endif

#if TRACE_EQUAL_PRIORITIES
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

enum { TRACE_TASK_COUNT = 3 };

static TaskHandle_t traceTaskHandles[TRACE_TASK_COUNT];

typedef struct {
    uint32_t delayBlocks;
    uint32_t mutexBlocks;
    TickType_t lastDelayAt;
    TickType_t lastMutexWaitAt;
} TaskTrace;

static TaskTrace taskTrace[TRACE_TASK_COUNT];

static void record_block(bool waitingForMutex)
{
    const TaskHandle_t current = xTaskGetCurrentTaskHandle();

    for (unsigned int i = 0; i < TRACE_TASK_COUNT; ++i) {
        if (traceTaskHandles[i] != NULL &&
            current == traceTaskHandles[i]) {
            const TickType_t now = xTaskGetTickCount();

            if (waitingForMutex) {
                ++taskTrace[i].mutexBlocks;
                taskTrace[i].lastMutexWaitAt = now;
            } else {
                ++taskTrace[i].delayBlocks;
                taskTrace[i].lastDelayAt = now;
            }

            return;
        }
    }
}

void trace_record_delay(void)
{
    record_block(false);
}

void trace_record_queue_receive(const void *queue)
{
    if (ledMutex != NULL && queue == (const void *)ledMutex) {
        record_block(true);
    }
}

static void trace_write_count(uint32_t value)
{
    char digits[11];
    char *position = &digits[10];
    *position = '\0';

    do {
        *--position = (char)('0' + value % 10U);
        value /= 10U;
    } while (value != 0U);

    board_uart_write(position);
}

static void trace_report(void)
{
    static const char *const names[TRACE_TASK_COUNT] = {
        "LED_ON", "LED_OFF", "UART"
    };
    static TaskTrace previous[TRACE_TASK_COUNT];
    static TickType_t previousReport;
    TaskTrace snapshot[TRACE_TASK_COUNT];
    TickType_t now;

    taskENTER_CRITICAL();
    now = xTaskGetTickCount();
    for (unsigned int i = 0; i < TRACE_TASK_COUNT; ++i) {
        snapshot[i] = taskTrace[i];
    }
    taskEXIT_CRITICAL();

#if TRACE_EQUAL_PRIORITIES
    board_uart_write("TRACE EQUAL 1/1/1 | Window: ");
#else
    board_uart_write("TRACE DISTINCT 3/2/1 | Window: ");
#endif

    uart_write_ms((TickType_t)(now - previousReport));
    board_uart_write("\r\n");
    previousReport = now;

    for (unsigned int i = 0; i < TRACE_TASK_COUNT; ++i) {
        const uint32_t delays =
            snapshot[i].delayBlocks - previous[i].delayBlocks;
        const uint32_t mutexWaits =
            snapshot[i].mutexBlocks - previous[i].mutexBlocks;

        board_uart_write(names[i]);
        board_uart_write(" | Delay blocks: ");
        trace_write_count(delays);

        if (delays != 0U) {
            board_uart_write(" | Last delay at: ");
            uart_write_ms(snapshot[i].lastDelayAt);
        }

        board_uart_write(" | Mutex blocks: ");
        trace_write_count(mutexWaits);

        if (mutexWaits != 0U) {
            board_uart_write(" | Last mutex wait at: ");
            uart_write_ms(snapshot[i].lastMutexWaitAt);
        }

        board_uart_write("\r\n");
        previous[i] = snapshot[i];
    }
}

static void task_uart(void *argument)
{
    (void)argument;

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
            }

            if (snapshot.haveOffDuration) {
                board_uart_write(" | last OFF: ");
                uart_write_ms(snapshot.offDuration);
            }

            board_uart_write("\r\n");
        }

        trace_report();
        vTaskDelay(pdMS_TO_TICKS(1000));;
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
                    ledOnPriority, &traceTaskHandles[0]) != pdPASS ||
        xTaskCreate(task_led_off, "LED_OFF", 256, NULL,
                    ledOffPriority, &traceTaskHandles[1]) != pdPASS ||
        xTaskCreate(task_uart, "UART", 256, NULL,
                    uartPriority, &traceTaskHandles[2]) != pdPASS) {
        board_uart_write("Task creation failed\r\n");
        for (;;) {}
    }

    vTaskStartScheduler();

    board_uart_write("Scheduler failed to start\r\n");
    for (;;) {}
}