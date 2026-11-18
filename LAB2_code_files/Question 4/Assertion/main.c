
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"

#include "experiment.h"
#include "board.h"

// ========================================
// FREERTOS OBJECTS
// ========================================

static SemaphoreHandle_t ledMutex = NULL;
static SemaphoreHandle_t turnOn = NULL;
static SemaphoreHandle_t turnOff = NULL;

static QueueHandle_t logQueue = NULL;

static TaskHandle_t task1Handle = NULL;
static TaskHandle_t task2Handle = NULL;

// ========================================
// LOGGING STRUCTURE
// ========================================

typedef struct
{
    TickType_t timestamp;
    char message[96];
} LogRecord;

// Logger function declaration
static void AppLog(const char *format, ...);

// ========================================
// OFFICIAL FREERTOS LOGGING CONFIGURATION
// ========================================

#include "logging_levels.h"

#define LIBRARY_LOG_NAME  "MKRZeroLab"
#define LIBRARY_LOG_LEVEL LOG_DEBUG

#define SdkLog(message) AppLog message

#include "logging_stack.h"

// ========================================
// TIMING VARIABLES
// ========================================

static TickType_t lastOnTime = 0;
static TickType_t lastOffTime = 0;

static int firstCycle = 1;

// Convert FreeRTOS ticks to milliseconds
static unsigned long ticksToMs(TickType_t ticks)
{
    return (unsigned long)
        (((uint64_t)ticks * 1000ULL)
         / configTICK_RATE_HZ);
}

// ========================================
// LOGGER FUNCTION
// ========================================

static void AppLog(const char *format, ...)
{
    if (logQueue == NULL)
    {
        return;
    }

    LogRecord record;

    record.timestamp = xTaskGetTickCount();

    va_list args;
    va_start(args, format);

    vsnprintf(
        record.message,
        sizeof(record.message),
        format,
        args
    );

    va_end(args);

    // Send message without blocking the task
    (void)xQueueSend(logQueue, &record, 0);
}

// ========================================
// TASK 1: LED ON FOR 500 ms
// ========================================

static void Task1_LED_ON(void *parameter)
{
    (void)parameter;

    for (;;)
    {
        LogDebug((
            "Task 1 waiting for ON signal"
        ));

        // Wait until Task 2 allows LED ON
        if (xSemaphoreTake(
                turnOn,
                portMAX_DELAY) == pdTRUE)
        {
            LogDebug((
                "Task 1 received ON signal"
            ));

            // Acquire the LED mutex
            if (xSemaphoreTake(
                    ledMutex,
                    portMAX_DELAY) == pdTRUE)
            {
                LogDebug((
                    "Task 1 acquired LED mutex"
                ));

                // Critical section
                board_led_on();

                lastOnTime = xTaskGetTickCount();

                LogInfo((
                    "LED ON at %lu ms",
                    ticksToMs(lastOnTime)
                ));

                // Measure previous OFF duration
                if (!firstCycle)
                {
                    LogInfo((
                        "OFF duration = %lu ms",
                        ticksToMs(
                            lastOnTime - lastOffTime
                        )
                    ));
                }

                // Release mutex
                BaseType_t result =
                    xSemaphoreGive(ledMutex);

                configASSERT(result == pdTRUE);
            }
            else
            {
                LogWarn((
                    "Task 1 failed to acquire mutex"
                ));

                continue;
            }

            // Active wait for 500 ms
            // This is NOT a blocked state
            TickType_t start =
                xTaskGetTickCount();

            while (
                (TickType_t)(
                    xTaskGetTickCount() - start
                ) < pdMS_TO_TICKS(LED_ON_MS))
            {
                // Busy waiting
            }

            LogDebug((
                "Task 1 completed active wait"
            ));

            // Allow Task 2 to turn LED OFF
            BaseType_t result =
                xSemaphoreGive(turnOff);

            configASSERT(result == pdTRUE);

            LogDebug((
                "Task 1 calling taskYIELD"
            ));

            taskYIELD();
        }
        else
        {
            LogWarn((
                "Task 1 failed to receive ON signal"
            ));
        }
    }
}

// ========================================
// TASK 2: LED OFF FOR 1000 ms
// ========================================

static void Task2_LED_OFF(void *parameter)
{
    (void)parameter;

    for (;;)
    {
        LogDebug((
            "Task 2 waiting for OFF signal"
        ));

        // Wait until Task 1 completes
        if (xSemaphoreTake(
                turnOff,
                portMAX_DELAY) == pdTRUE)
        {
            LogDebug((
                "Task 2 received OFF signal"
            ));

            // Acquire LED mutex
            if (xSemaphoreTake(
                    ledMutex,
                    portMAX_DELAY) == pdTRUE)
            {
                LogDebug((
                    "Task 2 acquired LED mutex"
                ));

                // Critical section
                board_led_off();

                lastOffTime = xTaskGetTickCount();

                LogInfo((
                    "LED OFF at %lu ms",
                    ticksToMs(lastOffTime)
                ));

                // Measure previous ON duration
                LogInfo((
                    "ON duration = %lu ms",
                    ticksToMs(
                        lastOffTime - lastOnTime
                    )
                ));

                firstCycle = 0;

                // Release mutex
                BaseType_t result =
                    xSemaphoreGive(ledMutex);

                configASSERT(result == pdTRUE);
            }
            else
            {
                LogWarn((
                    "Task 2 failed to acquire mutex"
                ));

                continue;
            }

            LogDebug((
                "Task 2 entering 1000 ms delay"
            ));

            // Task 2 enters the Blocked state
            vTaskDelay(
                pdMS_TO_TICKS(LED_OFF_MS)
            );

            LogDebug((
                "Task 2 resumed after delay"
            ));

            // Allow Task 1 to turn LED ON
            BaseType_t result =
                xSemaphoreGive(turnOn);

            configASSERT(result == pdTRUE);
        }
        else
        {
            LogWarn((
                "Task 2 failed to receive OFF signal"
            ));
        }
    }
}

// ========================================
// TASK STATE FUNCTION
// ========================================

static const char *taskStateName(
    eTaskState state)
{
    switch (state)
    {
        case eRunning:
            return "RUNNING";

        case eReady:
            return "READY";

        case eBlocked:
            return "BLOCKED";

        case eSuspended:
            return "SUSPENDED";

        case eDeleted:
            return "DELETED";

        default:
            return "UNKNOWN";
    }
}

// ========================================
// TASK 3: SERIAL LOGGING
// ========================================


static void Task3_LOGGER(void *parameter)
{
    (void)parameter;

    LogRecord record;
    char output[160];

    board_uart_write("\r\n");
    board_uart_write("FREERTOS TASK MONITOR\r\n");
    board_uart_write(EXPERIMENT_NAME);
    board_uart_write("\r\n");

    for (;;)
    {
        // Print the queued log messages
        while (xQueueReceive(logQueue,
                             &record, 0) == pdTRUE)
        {
            snprintf(output, sizeof(output),
                     "[%lu ms] %s\r\n",
                     ticksToMs(record.timestamp),
                     record.message);

            board_uart_write(output);
        }

        // Check the current states of tasks 1 and 2
        eTaskState state1 =
            eTaskGetState(task1Handle);

        eTaskState state2 =
            eTaskGetState(task2Handle);

        board_uart_write("\r\n--- TASK STATUS ---\r\n");

        snprintf(output, sizeof(output),
                 "Task 1 (LED ON): %s\r\n",
                 taskStateName(state1));
        board_uart_write(output);

        snprintf(output, sizeof(output),
                 "Task 2 (LED OFF): %s\r\n",
                 taskStateName(state2));
        board_uart_write(output);

        board_uart_write(
            "Task 3 (LOGGER): RUNNING\r\n"
        );

        if (state1 == eBlocked)
        {
            board_uart_write(
                ">>> TASK 1 IS BLOCKED\r\n"
            );
        }

        if (state2 == eBlocked)
        {
            board_uart_write(
                ">>> TASK 2 IS BLOCKED\r\n"
            );
        }

        // Check for indefinite semaphore waits.
        // FreeRTOS may report these as eSuspended.
        if (state1 == eSuspended)
        {
            board_uart_write(
                ">>> TASK 1 IS WAITING INDEFINITELY "
                "OR SUSPENDED\r\n"
            );
        }

        if (state2 == eSuspended)
        {
            board_uart_write(
                ">>> TASK 2 IS WAITING INDEFINITELY "
                "OR SUSPENDED\r\n"
            );
        }

        board_uart_write("-------------------\r\n");

        // Logger task blocks for 1 second
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

// ========================================
// MAIN FUNCTION
// ========================================

int main(void)
{
    // Initialize SAMD21 GPIO and UART
    board_init();

    // Create the mutex
    ledMutex = xSemaphoreCreateMutex();

    // Create binary semaphores
    turnOn = xSemaphoreCreateBinary();
    turnOff = xSemaphoreCreateBinary();

    // Create logging queue
    logQueue = xQueueCreate(
        24,
        sizeof(LogRecord)
    );

    // Check object creation
    configASSERT(ledMutex != NULL);
    configASSERT(turnOn != NULL);
    configASSERT(turnOff != NULL);
    configASSERT(logQueue != NULL);

    if (ledMutex == NULL ||
        turnOn == NULL ||
        turnOff == NULL ||
        logQueue == NULL)
    {
        for (;;) {}
    }

    // Create Task 1
    BaseType_t task1Result = xTaskCreate(
        Task1_LED_ON,
        "LED_ON",
        256,
        NULL,
        TASK1_PRIORITY,
        &task1Handle
    );

    // Create Task 2
    BaseType_t task2Result = xTaskCreate(
        Task2_LED_OFF,
        "LED_OFF",
        256,
        NULL,
        TASK2_PRIORITY,
        &task2Handle
    );

    // Create Task 3
    BaseType_t task3Result = xTaskCreate(
        Task3_LOGGER,
        "LOGGER",
        384,
        NULL,
        TASK3_PRIORITY,
        NULL
    );

    // Check task creation
    configASSERT(task1Result == pdPASS);
    configASSERT(task2Result == pdPASS);
    configASSERT(task3Result == pdPASS);

    if (task1Result != pdPASS ||
        task2Result != pdPASS ||
        task3Result != pdPASS)
    {
        for (;;) {}
    }

    // Begin first LED ON cycle
    BaseType_t result =
        xSemaphoreGive(turnOn);

    configASSERT(result == pdTRUE);

    // Start FreeRTOS scheduler
    vTaskStartScheduler();

    // Should never reach here
    for (;;) {}
}
