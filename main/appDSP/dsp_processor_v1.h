//
// Created by user on 26.10.26.
//

#ifndef MICROPLASTIC_DSP_PROCESSOR_V1_H
#define MICROPLASTIC_DSP_PROCESSOR_V1_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#define ROWS 32
#define COLS 10

/*
 * Вхідний формат повністю відповідає структурі збору даних користувача.
 * Порядок каналів у рядку: F1..F8, Clear, NIR (NIR має індекс 9).
 */
typedef struct __attribute__((packed)) {
    // TODO прибрати температуру взагалі!!
    float temp_dark;
    float temp_ir;
    float temp_blue;
    uint16_t dark_buf[ROWS][COLS];
    uint16_t ir_buf[ROWS][COLS];
    uint16_t blue_buf[ROWS][COLS];
} SensorData;

typedef enum {
    DSP_RESULT_UNINITIALIZED = 0,
    DSP_RESULT_MICROPLASTICS_DETECTED,
    DSP_RESULT_MICROPLASTICS_NOT_DETECTED,
    DSP_RESULT_ERROR_INVALID_INPUT,
    DSP_RESULT_ERROR_ADC_SATURATION,
    DSP_RESULT_ERROR_NUMERIC
} DspResultStatus;

typedef enum {
    DSP_DOMINANT_UNDETERMINED = 0,
    DSP_DOMINANT_NONPOLAR,
    DSP_DOMINANT_POLAR
} DspDominantFraction;

enum {
    DSP_FLAG_NONE                       = 0,
    DSP_FLAG_PI_DISAGREES_WITH_NNLS      = 1U << 0,
    DSP_FLAG_NONPOLAR_BELOW_LOD          = 1U << 1,
    DSP_FLAG_POLAR_BELOW_LOD             = 1U << 2
};

/*
 * Елемент вихідної черги. Сирі вхідні кадри копіюються сюди без змін,
 * щоб результат можна було зберегти або повторно обробити пізніше.
 */
typedef struct __attribute__((packed)) {
    DspResultStatus status;
    uint32_t flags;

    float concentration_total_mg_l;
    float concentration_nonpolar_mg_l;
    float concentration_polar_mg_l;
    float percent_nonpolar;
    float percent_polar;
    DspDominantFraction dominant_fraction;

    /* PI — спектральний діагностичний індекс, не класифікатор. */
    float polarity_index;
    float estimated_turbidity_ntu;
    float turbidity_signal_counts;
    float ife_multiplier;
    float lod_nonpolar_mg_l;
    float lod_polar_mg_l;

    /* Залишено місце під timestamp; зараз він не генерується. */
    uint64_t timestamp;
    bool timestamp_valid;

    SensorData raw;
} DspResult;

/*
 * Контекст для dsp_processing_task().
 * input_queue має містити елементи SensorData;
 * result_queue має містити елементи DspResult.
 */
typedef struct {
    QueueHandle_t input_queue;
    QueueHandle_t result_queue;
} DspTaskContext;

/*
 * Обробляє один повний набір DARK / IR / BLUE.
 * У разі помилки status у result вказує її тип, а функція повертає esp_err_t.
 * Калібровка задається константами у dsp_processor_v1.c.
 */
esp_err_t dsp_process_sensor_data(const SensorData *input, DspResult *result);

/*
 * FreeRTOS task: читає SensorData з input_queue, викликає DSP,
 * і відправляє DspResult (разом із сирими даними) у result_queue.
 */
void dsp_processing_task(void *pvParameters);


/*
 * 1. Виділити більший розмір стеку
 */



#endif //MICROPLASTIC_DSP_PROCESSOR_V1_H
