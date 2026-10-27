//
// Created by user on 26.10.26.
//

#include "dsp_processor_v1.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

#include "esp_log.h"
#include "dsps_dotprod.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#define TAG "WATER_DSP"

#define ADC_CLIP_LIMIT 18000U
#define NIR_CHANNEL_INDEX 9
#define VISIBLE_FIRST_INDEX 3
#define VISIBLE_CHANNEL_COUNT 5
#define PI_THRESHOLD 0.85f
#define IFE_MAX_MULTIPLIER 5.0f
#define LOD_SIGMA_MULTIPLIER 3.0f

/*
 * Ручна калібровка, перенесена з ir_pipeline_calibration_*.json.
 * Увага: файл JSON позначає ці значення як simulator-only. Це не заміна
 * калібровки реального AS7341, IR LED, оптики й еталонних NTU-зразків.
 */
typedef struct {
    float m_inv_5x5[5][5];
    float e_basis_5x2[5][2];
    float nir_counts_per_ntu;
    float ife_gain_decades_per_ntu;
    float alpha_dark_per_c;
    float sigma_min_counts;
    float k_nonpolar_mg_l;
    float k_polar_mg_l;
} DspCalibration;

static const DspCalibration s_calibration = {
    .m_inv_5x5 = {
        {1.0f, 0.0f, 0.0f, 0.0f, 0.0f},
        {0.0f, 1.0f, 0.0f, 0.0f, 0.0f},
        {0.0f, 0.0f, 1.0f, 0.0f, 0.0f},
        {0.0f, 0.0f, 0.0f, 1.0f, 0.0f},
        {0.0f, 0.0f, 0.0f, 0.0f, 1.0f},
    },
    .e_basis_5x2 = {
        {95.76660358930377f, 5.957651524329082f},
        {168.93518854366798f, 43.44751376930874f},
        {92.39052291043389f, 137.65756199270248f},
        {18.834958157268552f, 132.4014156689463f},
        {0.2944612951410614f, 15.79329610003891f},
    },
    .nir_counts_per_ntu = 120.0f,
    .ife_gain_decades_per_ntu = 0.00044216167867990193f,
    .alpha_dark_per_c = 0.010000000000000002f,
    .sigma_min_counts = 3.578467845468701f,
    .k_nonpolar_mg_l = 1.0f,
    .k_polar_mg_l = 1.0f,
};

/* ESP-DSP dot product wrapper used by matrix operations and NNLS. */
static bool dsp_dot(const float *a, const float *b, float *out, int length)
{
    if (a == NULL || b == NULL || out == NULL || length <= 0) {
        return false;
    }

    /* ESP-DSP dot products accumulate into dest instead of clearing it. */
    *out = 0.0f;
    if (dsps_dotprod_f32(a, b, out, length) != ESP_OK || !isfinite(*out)) {
        return false;
    }
    return true;
}

/*
 * Fixed-size row-major matrix multiplication for matrices up to 5x5.
 * ESP-DSP computes each row/column dot product; the dimensions here are
 * intentionally small and fixed by the calibration model.
 */
static bool matrix_multiply(const float *a, const float *b, float *out,
                            int rows_a, int cols_a, int cols_b)
{
    float row[5];
    float column[5];

    if (a == NULL || b == NULL || out == NULL ||
        rows_a < 1 || rows_a > 5 || cols_a < 1 || cols_a > 5 ||
        cols_b < 1 || cols_b > 5) {
        return false;
    }

    for (int r = 0; r < rows_a; ++r) {
        for (int c = 0; c < cols_b; ++c) {
            for (int k = 0; k < cols_a; ++k) {
                row[k] = a[r * cols_a + k];
                column[k] = b[k * cols_b + c];
            }
            if (!dsp_dot(row, column, &out[r * cols_b + c], cols_a)) {
                return false;
            }
        }
    }
    return true;
}

static bool calibration_is_valid(void)
{
    const DspCalibration *c = &s_calibration;
    float gram00 = 0.0f;
    float gram01 = 0.0f;
    float gram11 = 0.0f;
    float e0[5];
    float e1[5];

    if (!isfinite(c->nir_counts_per_ntu) || c->nir_counts_per_ntu <= 0.0f ||
        !isfinite(c->ife_gain_decades_per_ntu) ||
        c->ife_gain_decades_per_ntu < 0.0f ||
        !isfinite(c->alpha_dark_per_c) ||
        !isfinite(c->sigma_min_counts) || c->sigma_min_counts <= 0.0f ||
        !isfinite(c->k_nonpolar_mg_l) || c->k_nonpolar_mg_l <= 0.0f ||
        !isfinite(c->k_polar_mg_l) || c->k_polar_mg_l <= 0.0f) {
        return false;
    }

    for (int r = 0; r < 5; ++r) {
        for (int col = 0; col < 5; ++col) {
            if (!isfinite(c->m_inv_5x5[r][col])) {
                return false;
            }
        }
        e0[r] = c->e_basis_5x2[r][0];
        e1[r] = c->e_basis_5x2[r][1];
        if (!isfinite(e0[r]) || !isfinite(e1[r])) {
            return false;
        }
    }

    if (!dsp_dot(e0, e0, &gram00, 5) ||
        !dsp_dot(e0, e1, &gram01, 5) ||
        !dsp_dot(e1, e1, &gram11, 5)) {
        return false;
    }

    /* Non-zero Gram determinant means the two calibrated spectra are independent. */
    return (gram00 * gram11 - gram01 * gram01) > 1.0e-6f;
}

static bool sensor_data_is_valid(const SensorData *input, bool *saturated)
{
    const uint16_t *buffers[] = {
        &input->dark_buf[0][0],
        &input->ir_buf[0][0],
        &input->blue_buf[0][0],
    };

    *saturated = false;
    if (!isfinite(input->temp_dark) || !isfinite(input->temp_ir) ||
        !isfinite(input->temp_blue)) {
        return false;
    }

    for (size_t b = 0; b < sizeof(buffers) / sizeof(buffers[0]); ++b) {
        for (int i = 0; i < ROWS * COLS; ++i) {
            if (buffers[b][i] >= ADC_CLIP_LIMIT) {
                *saturated = true;
                return true;
            }
        }
    }
    return true;
}

/*
 * Для кожного каналу рахуємо середнє та SEM.
 * SEM = max(sample_stddev, sigma_min) / sqrt(N), як у Python DSP v4.
 */
static void calculate_mean_and_sem(const uint16_t buffer[ROWS][COLS],
                                   float mean[COLS], float sem[COLS])
{
    const float sqrt_n = sqrtf((float)ROWS);
    for (int col = 0; col < COLS; ++col) {
        float sum = 0.0f;
        for (int row = 0; row < ROWS; ++row) {
            sum += (float)buffer[row][col];
        }
        mean[col] = sum / (float)ROWS;

        float sum_squared_deviation = 0.0f;
        for (int row = 0; row < ROWS; ++row) {
            const float delta = (float)buffer[row][col] - mean[col];
            sum_squared_deviation += delta * delta;
        }

        /* Python uses ddof=1, тобто ділимо на N-1 для оцінки sample stddev. */
        const float sample_stddev = sqrtf(sum_squared_deviation / (float)(ROWS - 1));
        const float bounded_stddev =
            sample_stddev > s_calibration.sigma_min_counts
                ? sample_stddev
                : s_calibration.sigma_min_counts;
        sem[col] = bounded_stddev / sqrt_n;
    }
}

/*
 * NNLS для двох невід'ємних компонентів. Перевіряємо:
 * 1) повний unconstrained least-squares розв'язок, якщо обидва коефіцієнти >= 0;
 * 2) оптимум на кожній осі;
 * 3) нульовий розв'язок.
 * Це відповідає 2D NNLS-реалізації у Python v4.
 */
static bool solve_two_component_nnls(const float signal[5], float amplitude[2])
{
    float e0[5];
    float e1[5];
    /* У найгіршому разі маємо нуль, повний розв'язок і дві грані NNLS. */
    float candidates[4][2] = {
        {0.0f, 0.0f}, {0.0f, 0.0f}, {0.0f, 0.0f}, {0.0f, 0.0f}
    };
    float gram00, gram01, gram11, rhs0, rhs1;
    float best_error = INFINITY;
    int candidate_count = 1;

    for (int r = 0; r < 5; ++r) {
        e0[r] = s_calibration.e_basis_5x2[r][0];
        e1[r] = s_calibration.e_basis_5x2[r][1];
    }

    if (!dsp_dot(e0, e0, &gram00, 5) ||
        !dsp_dot(e0, e1, &gram01, 5) ||
        !dsp_dot(e1, e1, &gram11, 5) ||
        !dsp_dot(e0, signal, &rhs0, 5) ||
        !dsp_dot(e1, signal, &rhs1, 5)) {
        return false;
    }

    const float determinant = gram00 * gram11 - gram01 * gram01;
    if (determinant <= 1.0e-6f || !isfinite(determinant)) {
        return false;
    }

    const float full0 = (rhs0 * gram11 - rhs1 * gram01) / determinant;
    const float full1 = (rhs1 * gram00 - rhs0 * gram01) / determinant;
    if (full0 >= 0.0f && full1 >= 0.0f) {
        candidates[candidate_count][0] = full0;
        candidates[candidate_count][1] = full1;
        ++candidate_count;
    }

    candidates[candidate_count][0] = fmaxf(0.0f, rhs0 / gram00);
    candidates[candidate_count][1] = 0.0f;
    ++candidate_count;
    candidates[candidate_count][0] = 0.0f;
    candidates[candidate_count][1] = fmaxf(0.0f, rhs1 / gram11);
    ++candidate_count;

    for (int c = 0; c < candidate_count; ++c) {
        float residual[5];
        float modeled[5];
        float squared_error;

        for (int r = 0; r < 5; ++r) {
            modeled[r] = s_calibration.e_basis_5x2[r][0] * candidates[c][0] +
                         s_calibration.e_basis_5x2[r][1] * candidates[c][1];
            residual[r] = modeled[r] - signal[r];
        }
        if (!dsp_dot(residual, residual, &squared_error, 5)) {
            return false;
        }
        if (squared_error < best_error) {
            best_error = squared_error;
            amplitude[0] = candidates[c][0];
            amplitude[1] = candidates[c][1];
        }
    }
    return isfinite(amplitude[0]) && isfinite(amplitude[1]);
}

/*
 * Обчислює 3-sigma межі виявлення через ту саму лінійну коваріаційну
 * апроксимацію, що й Python v4: M_inv -> E псевдообернена -> амплітуди.
 */
static bool calculate_lod(const float sem_dark[COLS],
                          const float sem_blue[COLS],
                          float dark_to_blue_factor,
                          float ife_multiplier,
                          float *lod_nonpolar,
                          float *lod_polar)
{
    float raw_variance[5];
    float transform[5][5];
    float transform_transposed[5][5];
    float weighted_transform[5][5];
    float latent_covariance[5][5];
    float gram[2][2] = {{0.0f}};
    float gram_inverse[2][2];
    float decoder[2][5];
    float decoder_transposed[5][2];
    float intermediate[2][5];
    float amplitude_covariance[2][2];
    float e_col0[5];
    float e_col1[5];
    float determinant;

    for (int i = 0; i < 5; ++i) {
        const int channel = VISIBLE_FIRST_INDEX + i;
        raw_variance[i] =
            sem_blue[channel] * sem_blue[channel] +
            sem_dark[channel] * sem_dark[channel] *
                dark_to_blue_factor * dark_to_blue_factor;

        e_col0[i] = s_calibration.e_basis_5x2[i][0];
        e_col1[i] = s_calibration.e_basis_5x2[i][1];
    }

    if (!dsp_dot(e_col0, e_col0, &gram[0][0], 5) ||
        !dsp_dot(e_col0, e_col1, &gram[0][1], 5) ||
        !dsp_dot(e_col1, e_col1, &gram[1][1], 5)) {
        return false;
    }
    gram[1][0] = gram[0][1];
    determinant = gram[0][0] * gram[1][1] - gram[0][1] * gram[1][0];
    if (determinant <= 1.0e-6f || !isfinite(determinant)) {
        return false;
    }

    /* (E^T E)^-1 */
    gram_inverse[0][0] = gram[1][1] / determinant;
    gram_inverse[0][1] = -gram[0][1] / determinant;
    gram_inverse[1][0] = -gram[1][0] / determinant;
    gram_inverse[1][1] = gram[0][0] / determinant;

    /*
     * Decoder = pinv(E) = (E^T E)^-1 E^T, розмір 2x5.
     * E зберігається як 5x2, тому транспоновані елементи читаємо явно.
     */
    for (int r = 0; r < 2; ++r) {
        for (int c = 0; c < 5; ++c) {
            decoder[r][c] =
                gram_inverse[r][0] * s_calibration.e_basis_5x2[c][0] +
                gram_inverse[r][1] * s_calibration.e_basis_5x2[c][1];
        }
    }
    for (int r = 0; r < 5; ++r) {
        for (int c = 0; c < 2; ++c) {
            decoder_transposed[r][c] = decoder[c][r];
        }
    }

    /* Джейкобіан перетворення виміряних видимих каналів у latent-сигнал. */
    for (int r = 0; r < 5; ++r) {
        for (int c = 0; c < 5; ++c) {
            transform[r][c] =
                ife_multiplier * s_calibration.m_inv_5x5[r][c];
            transform_transposed[c][r] = transform[r][c];
            weighted_transform[r][c] = transform[r][c] * raw_variance[c];
        }
    }

    if (!matrix_multiply(&weighted_transform[0][0],
                         &transform_transposed[0][0],
                         &latent_covariance[0][0], 5, 5, 5) ||
        !matrix_multiply(&decoder[0][0], &latent_covariance[0][0],
                         &intermediate[0][0], 2, 5, 5) ||
        !matrix_multiply(&intermediate[0][0], &decoder_transposed[0][0],
                         &amplitude_covariance[0][0], 2, 5, 2)) {
        return false;
    }

    *lod_nonpolar = LOD_SIGMA_MULTIPLIER *
        s_calibration.k_nonpolar_mg_l *
        sqrtf(fmaxf(0.0f, amplitude_covariance[0][0]));
    *lod_polar = LOD_SIGMA_MULTIPLIER *
        s_calibration.k_polar_mg_l *
        sqrtf(fmaxf(0.0f, amplitude_covariance[1][1]));

    return isfinite(*lod_nonpolar) && isfinite(*lod_polar);
}

static void set_error_result(DspResult *result, DspResultStatus status)
{
    result->status = status;
    result->flags = DSP_FLAG_NONE;
}

esp_err_t dsp_process_sensor_data(const SensorData *input, DspResult *result)
{
    float mean_dark[COLS], mean_ir[COLS], mean_blue[COLS];
    float sem_dark[COLS], sem_ir[COLS], sem_blue[COLS];
    float dark_to_ir_factor;
    float dark_to_blue_factor;
    float ir_signal;
    float ntu;
    float blue_visible[5];
    float latent[5];
    float corrected[5];
    float amplitudes[2] = {0.0f, 0.0f};
    float total;
    float exponent;
    float ife_multiplier;
    float lod_nonpolar;
    float lod_polar;
    bool saturated;

    if (result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(result, 0, sizeof(*result));
    result->status = DSP_RESULT_ERROR_INVALID_INPUT;

    if (input == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    result->raw = *input;

    if (!sensor_data_is_valid(input, &saturated)) {
        ESP_LOGE(TAG, "Вхід відхилено: температурне поле NaN/Inf.");
        set_error_result(result, DSP_RESULT_ERROR_INVALID_INPUT);
        return ESP_ERR_INVALID_ARG;
    }
    if (saturated) {
        ESP_LOGE(TAG, "Вхід відхилено: знайдено відлік ADC >= %u.",
                 (unsigned)ADC_CLIP_LIMIT);
        set_error_result(result, DSP_RESULT_ERROR_ADC_SATURATION);
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (!calibration_is_valid()) {
        ESP_LOGE(TAG, "Некоректна вбудована калібровка DSP.");
        set_error_result(result, DSP_RESULT_ERROR_NUMERIC);
        return ESP_ERR_INVALID_STATE;
    }

    /* Крок 1: оцінюємо середні значення каналів і SEM по 32 кадрах. */
    calculate_mean_and_sem(input->dark_buf, mean_dark, sem_dark);
    calculate_mean_and_sem(input->ir_buf, mean_ir, sem_ir);
    calculate_mean_and_sem(input->blue_buf, mean_blue, sem_blue);

    /* Крок 2: локальна лінійна поправка темнового рівня за температурою. */
    dark_to_ir_factor =
        1.0f + s_calibration.alpha_dark_per_c *
                   (input->temp_ir - input->temp_dark);
    dark_to_blue_factor =
        1.0f + s_calibration.alpha_dark_per_c *
                   (input->temp_blue - input->temp_dark);
    if (!isfinite(dark_to_ir_factor) || !isfinite(dark_to_blue_factor)) {
        set_error_result(result, DSP_RESULT_ERROR_NUMERIC);
        return ESP_ERR_INVALID_RESPONSE;
    }

    /*
     * Крок 3: каламутність отримується тільки з IR-фази, NIR-канал.
     * Віднімаємо температурно скоригований темновий NIR, обрізаємо
     * від'ємний результат до нуля й переводимо counts у NTU.
     */
    ir_signal = mean_ir[NIR_CHANNEL_INDEX] -
                mean_dark[NIR_CHANNEL_INDEX] * dark_to_ir_factor;
    if (!isfinite(ir_signal)) {
        set_error_result(result, DSP_RESULT_ERROR_NUMERIC);
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (ir_signal < 0.0f) {
        ir_signal = 0.0f;
    }
    ntu = ir_signal / s_calibration.nir_counts_per_ntu;

    /* Крок 4: коригуємо п'ять каліброваних видимих каналів BLUE-фази. */
    for (int i = 0; i < VISIBLE_CHANNEL_COUNT; ++i) {
        const int channel = VISIBLE_FIRST_INDEX + i;
        blue_visible[i] =
            mean_blue[channel] - mean_dark[channel] * dark_to_blue_factor;
    }
    if (!matrix_multiply(&s_calibration.m_inv_5x5[0][0],
                         blue_visible, latent, 5, 5, 1)) {
        set_error_result(result, DSP_RESULT_ERROR_NUMERIC);
        return ESP_FAIL;
    }

    /*
     * Крок 5: IFE-множник залежить від виміряної NTU.
     * Обмежуємо показник зверху до log10(IFE_MAX), що еквівалентно min()
     * у Python, але не дозволяє переповнити powf().
     */
    exponent = s_calibration.ife_gain_decades_per_ntu * ntu;
    const float max_exponent = log10f(IFE_MAX_MULTIPLIER);
    if (exponent > max_exponent) {
        exponent = max_exponent;
    }
    ife_multiplier = powf(10.0f, exponent);
    for (int i = 0; i < 5; ++i) {
        corrected[i] = latent[i] * ife_multiplier;
    }

    /* Крок 6: NNLS розкладає спектральний вектор на дві полярності. */
    if (!solve_two_component_nnls(corrected, amplitudes)) {
        set_error_result(result, DSP_RESULT_ERROR_NUMERIC);
        return ESP_FAIL;
    }
    result->concentration_nonpolar_mg_l =
        amplitudes[0] * s_calibration.k_nonpolar_mg_l;
    result->concentration_polar_mg_l =
        amplitudes[1] * s_calibration.k_polar_mg_l;
    total = result->concentration_nonpolar_mg_l +
            result->concentration_polar_mg_l;
    result->concentration_total_mg_l = total;

    /* Крок 7: відсоткова частка та домінантність беруться з NNLS. */
    if (total > 0.0f) {
        result->percent_nonpolar =
            100.0f * result->concentration_nonpolar_mg_l / total;
        result->percent_polar =
            100.0f * result->concentration_polar_mg_l / total;
        result->dominant_fraction =
            result->concentration_nonpolar_mg_l >=
                    result->concentration_polar_mg_l
                ? DSP_DOMINANT_NONPOLAR
                : DSP_DOMINANT_POLAR;
    } else {
        result->dominant_fraction = DSP_DOMINANT_UNDETERMINED;
    }
    result->polarity_index =
        fmaxf(0.0f, corrected[2]) / fmaxf(1.0e-6f, corrected[1]);

    /* PI — тільки діагностика, він не змінює клас, визначений NNLS. */
    if ((result->polarity_index >= PI_THRESHOLD &&
         result->concentration_nonpolar_mg_l <
             result->concentration_polar_mg_l) ||
        (result->polarity_index < PI_THRESHOLD &&
         result->concentration_polar_mg_l <
             result->concentration_nonpolar_mg_l)) {
        result->flags |= DSP_FLAG_PI_DISAGREES_WITH_NNLS;
    }

    /* Крок 8: рахуємо межі виявлення з шуму темнової та синьої фаз. */
    if (!calculate_lod(sem_dark, sem_blue, dark_to_blue_factor,
                       ife_multiplier, &lod_nonpolar, &lod_polar)) {
        set_error_result(result, DSP_RESULT_ERROR_NUMERIC);
        return ESP_FAIL;
    }
    result->lod_nonpolar_mg_l = lod_nonpolar;
    result->lod_polar_mg_l = lod_polar;
    if (result->concentration_nonpolar_mg_l < lod_nonpolar) {
        result->flags |= DSP_FLAG_NONPOLAR_BELOW_LOD;
    }
    if (result->concentration_polar_mg_l < lod_polar) {
        result->flags |= DSP_FLAG_POLAR_BELOW_LOD;
    }

    if (!isfinite(total) || !isfinite(result->percent_nonpolar) ||
        !isfinite(result->percent_polar) || !isfinite(result->polarity_index) ||
        !isfinite(ntu) || !isfinite(ife_multiplier)) {
        set_error_result(result, DSP_RESULT_ERROR_NUMERIC);
        return ESP_ERR_INVALID_RESPONSE;
    }

    result->estimated_turbidity_ntu = ntu;
    result->turbidity_signal_counts = ir_signal;
    result->ife_multiplier = ife_multiplier;
    result->status =
        ((result->concentration_nonpolar_mg_l >= lod_nonpolar ||
          result->concentration_polar_mg_l >= lod_polar) &&
         total > 0.0f)
            ? DSP_RESULT_MICROPLASTICS_DETECTED
            : DSP_RESULT_MICROPLASTICS_NOT_DETECTED;

    ESP_LOGI(TAG,
             "DSP: NTU=%.4f (IR/NIR net=%.3f counts), total=%.5f mg/L "
             "(NP=%.5f, P=%.5f), PI=%.4f, flags=0x%lx",
             (double)ntu, (double)ir_signal,
             (double)total,
             (double)result->concentration_nonpolar_mg_l,
             (double)result->concentration_polar_mg_l,
             (double)result->polarity_index,
             (unsigned long)result->flags);
    return ESP_OK;
}




void dsp_processing_task(void *pvParameters)
{
    DspTaskContext *context = (DspTaskContext *)pvParameters;

    /*if (context == NULL || context->input_queue == NULL ||
        context->result_queue == NULL) {
        ESP_LOGE(TAG, "Task DSP запущено з некоректним DspTaskContext.");
        vTaskDelete(NULL);
        return;
    }*/

    ESP_LOGI(TAG, "DSP task запущено; очікування SensorData у FreeRTOS queue.");
    for (;;) {
        SensorData input;
        DspResult result;

        input.temp_dark = 26.777500f;
        input.temp_blue = 27.252500f;
        input.temp_ir = 27.015000f;
        uint16_t darkBuf[32][10] = {
            { 5, 11, 12,  8, 15, 13, 17, 15,  5, 10 },
            { 5,  9, 11, 17, 17, 17, 14, 13, 10, 15 },
            {11, 14, 13, 13, 16,  9, 13,  5, 15, 12 },
            {15, 13, 11, 12,  8, 12,  9, 10, 10, 13 },
            {16,  8, 16, 10, 12,  9,  9, 12, 11, 17 },
            {12,  8, 15, 12,  7, 13, 13, 13, 13, 11 },
            {11, 16, 13,  8, 14, 11, 14, 11, 14, 11 },
            {13, 15,  9, 18, 12, 12, 15,  8,  9,  9 },
            {18, 11,  5, 14, 14, 13,  7, 15, 13, 15 },
            {14, 10,  9, 12, 14, 17, 18, 13, 12, 11 },
            {10, 12, 10, 13, 10,  7, 11, 10, 10,  9 },
            { 7, 13, 13, 13, 18, 14, 11, 13, 13, 13 },
            {12,  9, 14, 13, 12, 10, 13,  8, 11,  9 },
            { 1, 14, 13, 19, 13, 14, 13, 13, 15, 14 },
            {10, 14, 10, 13, 11, 12, 18, 16,  6,  9 },
            { 8, 11, 17, 13, 15, 14, 10, 15, 12,  9 },
            {11, 10, 13,  7,  9, 15, 13, 14, 14, 10 },
            { 9,  3, 12, 14, 13, 10, 11, 17, 10, 14 },
            {18, 15, 13, 18, 14, 13, 13, 11, 12, 11 },
            {14, 16, 13, 14,  6, 12, 12, 13,  9,  4 },
            { 9, 10, 20, 13,  6, 13, 10, 13, 14, 17 },
            {12, 17, 12, 14, 11,  6, 13, 16, 11, 12 },
            {15,  7, 13, 13, 14, 10, 15,  9, 11, 14 },
            {14,  7, 10,  9,  7, 14, 15, 15, 14, 14 },
            {14, 10, 15, 12,  9, 13, 10, 13, 10,  9 },
            {12,  8, 12,  7,  6, 14, 10,  7, 10,  7 },
            { 8,  9, 14, 11,  7, 11, 13, 10, 14,  3 },
            {17, 16,  9,  9, 14,  7, 13, 12, 12,  6 },
            { 9, 10,  6, 10, 13, 11, 11, 11,  7, 14 },
            {13, 13, 10, 16, 11, 14, 13, 16, 14,  8 },
            {11, 13,  9, 15, 12, 11, 16, 11, 15, 10 },
            {14, 12, 15, 11, 15, 10,  8, 11, 16, 14 }
        };

        uint16_t irBuf[32][10] = {
            {19, 18, 11, 10, 11, 14, 13, 13, 12, 16},
            {12, 14, 18, 12, 12,  9,  8, 10,  9, 16},
            {14, 16,  9,  4, 13, 11, 13, 12,  6, 15},
            { 9,  8, 11, 13, 13,  8, 15, 11, 12, 10},
            {12, 12, 10, 14, 11, 15, 15,  9, 11,  9},
            {17, 11, 11, 12, 12, 15,  7, 14, 10, 14},
            {11, 15, 15, 17,  7, 10, 11,  9, 14, 16},
            {13, 14, 18, 18, 16, 13, 12, 16, 11, 16},
            { 9, 14, 15,  9, 11, 17, 11, 11, 15, 16},
            {21, 12,  8, 15,  8,  9, 16, 12, 13, 12},
            {17, 11, 12, 10, 15, 14, 12, 13, 12, 17},
            {11, 14,  7, 16, 15, 18, 14, 13,  8, 11},
            {11, 15, 14, 12, 11,  9, 16, 15, 11, 19},
            {11, 12, 14, 11, 14,  9, 16, 15, 10, 15},
            {10, 11, 12, 18, 10, 15,  8, 11, 13, 11},
            { 8, 11, 15, 10, 15, 12,  9, 17, 15, 18},
            { 8, 14, 12, 15, 12,  6, 17, 16, 17, 10},
            {10, 12,  9,  9, 14, 13, 12,  7, 15, 12},
            {12, 12, 13,  9, 10,  9, 16, 15, 11, 13},
            {12, 11, 12, 13, 13, 13, 13, 10, 11, 16},
            {15, 13, 19, 12,  9, 13, 17, 14, 15, 12},
            {13, 10,  8, 17, 13, 13, 16, 15, 10, 12},
            { 8, 10, 10, 15, 13, 18, 15, 10, 11, 14},
            {14, 14, 17, 12, 13,  7, 10, 11, 12,  7},
            {12, 18,  8, 13,  7, 15,  8, 23, 14, 16},
            {10, 10,  8, 12, 14, 13,  9, 12, 15, 18},
            { 8, 15, 10, 11,  8, 13, 10, 11, 17, 16},
            {14, 15, 13, 14, 14, 10,  6,  8, 15, 10},
            { 9,  9, 11,  8,  9, 10, 13, 14, 13, 11},
            {11, 17,  8, 12, 18,  8, 10, 15, 13, 12},
            {13, 15,  7, 10, 18, 10, 12,  8, 12, 12},
            {10, 14, 14, 11, 10, 15,  4, 13,  6, 13}
        };

        uint16_t blueBuf[32][10] = {
            {14, 15, 22, 244, 514, 640, 452, 59, 1618, 10},
            {10, 11, 30, 242, 508, 641, 457, 62, 1625, 16},
            {12,  5, 24, 246, 501, 644, 461, 56, 1616, 12},
            { 9, 15, 30, 245, 511, 637, 461, 55, 1619, 17},
            {15, 14, 29, 246, 504, 642, 454, 63, 1621, 12},
            { 8, 12, 24, 246, 505, 642, 453, 63, 1625,  7},
            { 7, 13, 26, 251, 513, 643, 453, 59, 1618,  9},
            {14, 14, 32, 242, 508, 641, 457, 55, 1625, 14},
            {13, 16, 30, 249, 503, 641, 459, 64, 1619,  8},
            {14, 11, 23, 241, 506, 645, 461, 58, 1620, 12},
            {14, 14, 24, 250, 508, 649, 463, 62, 1630, 14},
            {12, 12, 26, 253, 509, 637, 452, 62, 1622, 10},
            {12,  7, 24, 244, 508, 647, 454, 68, 1623, 14},
            {11, 10, 25, 241, 503, 642, 461, 62, 1622, 11},
            {17,  9, 25, 252, 508, 638, 462, 58, 1634, 14},
            {11, 12, 30, 248, 507, 640, 456, 60, 1621, 14},
            {16, 14, 22, 249, 509, 641, 460, 58, 1619, 12},
            {15, 15, 26, 243, 505, 639, 456, 57, 1624, 17},
            {11, 16, 26, 251, 511, 644, 460, 61, 1623,  9},
            {15, 12, 23, 251, 518, 640, 458, 61, 1623,  7},
            {10,  9, 28, 249, 514, 646, 458, 62, 1625, 10},
            {15,  9, 27, 252, 509, 646, 451, 63, 1618, 15},
            {13, 14, 24, 251, 508, 644, 459, 67, 1619, 12},
            {11, 12, 27, 250, 513, 642, 462, 60, 1613, 12},
            {11, 16, 27, 244, 514, 638, 461, 61, 1623,  9},
            {14, 12, 25, 250, 506, 646, 460, 61, 1625, 14},
            {12, 13, 26, 253, 501, 642, 459, 62, 1626, 11},
            { 8,  6, 27, 251, 512, 637, 456, 62, 1619, 15},
            { 9, 15, 27, 250, 511, 633, 461, 57, 1622, 10},
            {15, 13, 27, 248, 506, 643, 453, 62, 1634, 15},
            {11, 12, 28, 253, 509, 643, 464, 62, 1624, 14},
            { 6, 10, 25, 251, 508, 639, 455, 64, 1622, 11}
        };
       memcpy(input.dark_buf, darkBuf, sizeof(input.dark_buf));
       memcpy(input.blue_buf, blueBuf, sizeof(input.blue_buf));
       memcpy(input.ir_buf, irBuf, sizeof(input.ir_buf));

        /*if (xQueueReceive(context->input_queue, &input, portMAX_DELAY) != pdTRUE) {
            continue;
        }*/

        memset(&result, 0, sizeof(result));
        result.raw = input;
        result.timestamp = 0;
        result.timestamp_valid = false;

        const esp_err_t err = dsp_process_sensor_data(&input, &result);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Обробка завершилась помилкою: %s (status=%d).",
                     esp_err_to_name(err), (int)result.status);
        }

        /*
         * Не відкидаємо вимірювання при помилці DSP: у чергу все одно
         * передається запис зі статусом помилки та копією сирих даних.
         */
        /*if (xQueueSend(context->result_queue, &result, portMAX_DELAY) != pdTRUE) {
            ESP_LOGE(TAG, "Не вдалося передати DspResult у вихідну queue.");
        }*/
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}