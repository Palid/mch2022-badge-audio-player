/*
 * This example code is in the Public Domain (or CC0 licensed, at your option.)
 * Unless required by applicable law or agreed to in writing, this
 * software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
 * CONDITIONS OF ANY KIND, either express or implied.
 */

#define SOC_I2S_SUPPORTS_ADC_DAC 1

#include "main.h"
#include "audio_alc.h"

#include <pthread.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_pthread.h"
#include <esp_log.h>

static void *audio_thread(void *arg);

static pax_buf_t buf;
xQueueHandle buttonQueue;
xQueueHandle audioQueue;

// Audio stuff
pthread_attr_t attr;
pthread_t thread1, thread2;
esp_pthread_cfg_t esp_pthread_cfg;
audio_element_handle_t alc_el;
// Make the standard volume bearable
int volume = -16;

static const char *TAG = "audio-player";

// Updates the screen with the latest buffer.
void disp_flush()
{
    ESP_LOGI(TAG, "FLUSHING DISPLAY");
    if (ili9341_write(get_ili9341(), buf.buf))
    {
        ESP_LOGI(TAG, "Display write failed.");
    }
    else
    {
        ESP_LOGI(TAG, "Display write succeed.");
    }
}

// Exits the app, returning to the launcher.
void exit_to_launcher()
{
    REG_WRITE(RTC_CNTL_STORE0_REG, 0);
    esp_restart();
}

void draw_center(char *text)
{

    // Pick the font (Saira is the only one that looks nice in this size).
    const pax_font_t *font = pax_font_saira_condensed;

    // Determine how the text dimensions so we can display it centered on
    // screen.
    pax_vec1_t dims = pax_text_size(font, font->default_size, text);

    // Draw the centered text.
    pax_draw_text(
        &buf,                     // Buffer to draw to.
        0xff000000,               // color
        font, font->default_size, // Font and size to use.
        // Position (top left corner) of the app.
        (buf.width - dims.x) / 2.0,
        (buf.height - dims.y) / 2.0,
        // The text to be rendered.
        text);
}

bool connect_to_wifi()
{
    bool success = wifi_connect_to_stored();
    char *text;
    if (success)
    {
        text = "Connected to wifi!";
        // Green color if connected successfully.
        pax_background(&buf, 0xff00ff00);
    }
    else
    {
        text = "Wifi not connected, should probably try again.";
        // Red color if not connected.
        pax_background(&buf, 0xffff0000);
    }
    draw_center(text);
    disp_flush();
    return success;
}

void app_main()
{
    ESP_LOGI(TAG, "Starting audio player");

    // Initialize the screen, the I2C and the SPI busses.
    bsp_init();

    // Initialize the RP2040 (responsible for buttons, etc).
    bsp_rp2040_init();

    // This queue is used to receive button presses.
    buttonQueue = get_rp2040()->queue;

    // Initialize graphics for the screen.
    pax_buf_init(&buf, NULL, 320, 240, PAX_BUF_16_565RGB);

    pax_col_t col = pax_col_hsv(0, 255 /*saturation*/, 255 /*brighness*/);
    pax_background(&buf, col);
    draw_center("LOADING");
    disp_flush();

    // Initialize NVS.
    nvs_flash_init();

    // Initialize WiFi. This doesn't connect to Wifi yet.
    wifi_init();

    // // Now, connect to WiFi using the stored settings.
    connect_to_wifi();

    int res = pthread_create(&thread1, NULL, audio_thread, NULL);
    assert(res == 0);
    ESP_LOGI(TAG, "[0] Created audio thread");

    while (1)
    {
        // Pick a random background color.
        int hue = esp_random() & 255;
        pax_col_t col = pax_col_hsv(hue, 255 /*saturation*/, 255 /*brighness*/);

        // Greet the World in front of a random background color!
        // Fill the background with the random color.
        pax_background(&buf, col);

        // This text is shown on screen.
        char *text = "Hello, MCH2033!";

        // Pick the font (Saira is the only one that looks nice in this size).
        const pax_font_t *font = pax_font_saira_condensed;

        // Determine how the text dimensions so we can display it centered on
        // screen.
        pax_vec1_t dims = pax_text_size(font, font->default_size, text);

        // Draw the centered text.
        pax_draw_text(
            &buf,                     // Buffer to draw to.
            0xff000000,               // color
            font, font->default_size, // Font and size to use.
            // Position (top left corner) of the app.
            (buf.width - dims.x) / 2.0,
            (buf.height - dims.y) / 2.0,
            // The text to be rendered.
            text);

        // Draws the entire graphics buffer to the screen.
        disp_flush();

        // Wait for button presses and do another cycle.

        // Structure used to receive data.
        rp2040_input_message_t message;

        // Wait forever for a button press (because of portMAX_DELAY)
        xQueueReceive(buttonQueue, &message, portMAX_DELAY);

        if (message.state)
        {
            int next = 0;
            switch (message.input)
            {
            case RP2040_INPUT_BUTTON_HOME:
                ESP_LOGI(TAG, "[BUTTON] Home button pressed.");
                // If home is pressed, exit to launcher.
                exit_to_launcher();
                break;
            case RP2040_INPUT_JOYSTICK_DOWN:
                volume = alc_volume_setup_get_volume(alc_el);
                next = volume - 5;
                if (next < -64)
                {
                    next = -64;
                }
                alc_volume_setup_set_volume(alc_el, next);
                break;
            case RP2040_INPUT_JOYSTICK_UP:
                volume = alc_volume_setup_get_volume(alc_el);
                next = volume + 5;
                if (next > 0)
                    next = 0;
                alc_volume_setup_set_volume(alc_el, next);
                break;
            }
        }
    }
}

static void *audio_thread(void *arg)
{

    audio_pipeline_handle_t pipeline;
    audio_element_handle_t http_stream_reader, i2s_stream_writer, mp3_decoder;

    esp_log_level_set("*", ESP_LOG_WARN);
    esp_log_level_set(TAG, ESP_LOG_DEBUG);

    ESP_LOGI(TAG, "[ 1 ] Start audio codec chip");
    /* I2S configuration parameters */
    i2s_config_t i2s_config = {
        .mode = I2S_MODE_MASTER | I2S_MODE_TX, // TX only
        .sample_rate = 44100,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT, // stereo
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .dma_buf_count = 6,
        .dma_buf_len = 128,
        .intr_alloc_flags = 0, // default interrupt priority
        .bits_per_chan = I2S_BITS_PER_SAMPLE_16BIT,
        .tx_desc_auto_clear = true // auto clear tx descriptor on underflow
    };
    i2s_driver_install(0, &i2s_config, 0, NULL);

    /* enable I2S */
    i2s_pin_config_t pin_config = {
        .mck_io_num = GPIO_I2S_MCLK,
        .bck_io_num = GPIO_I2S_CLK,
        .ws_io_num = GPIO_I2S_LR,
        .data_out_num = GPIO_I2S_DATA,
        .data_in_num = I2S_PIN_NO_CHANGE};
    i2s_set_pin(0, &pin_config);

    ESP_LOGI(TAG, "[2.0] Create audio pipeline for playback");
    audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    pipeline = audio_pipeline_init(&pipeline_cfg);
    mem_assert(pipeline);

    ESP_LOGI(TAG, "[2.1] Create http stream to read data");
    http_stream_cfg_t http_cfg = HTTP_STREAM_CFG_DEFAULT();
    http_stream_reader = http_stream_init(&http_cfg);

    ESP_LOGI(TAG, "[2.2] Create i2s stream to write data to codec chip");
    i2s_stream_cfg_t i2s_cfg = I2S_STREAM_CFG_DEFAULT();
    i2s_cfg.type = AUDIO_STREAM_WRITER;
    i2s_cfg.i2s_port = 0;
    i2s_stream_writer = i2s_stream_init(&i2s_cfg);

    ESP_LOGI(TAG, "[2.3] Create mp3 decoder to decode mp3 file");
    mp3_decoder_cfg_t mp3_cfg = DEFAULT_MP3_DECODER_CONFIG();
    mp3_decoder = mp3_decoder_init(&mp3_cfg);

    ESP_LOGI(TAG, "[2.3.5] Creating software ALC (software volume adjustment)");
    alc_volume_setup_cfg_t alc_cfg = DEFAULT_ALC_VOLUME_SETUP_CONFIG();
    alc_el = alc_volume_setup_init(&alc_cfg);
    alc_volume_setup_set_volume(alc_el, volume);

    ESP_LOGI(TAG, "[2.4] Register all elements to audio pipeline");
    audio_pipeline_register(pipeline, http_stream_reader, "http");
    audio_pipeline_register(pipeline, mp3_decoder, "mp3");
    audio_pipeline_register(pipeline, alc_el, "alc");
    audio_pipeline_register(pipeline, i2s_stream_writer, "i2s");

    ESP_LOGI(TAG, "[2.5] Link it together http_stream-->mp3_decoder-->i2s_stream-->[codec_chip]");
    const char *link_tag[4] = {"http", "mp3", "alc", "i2s"};
    audio_pipeline_link(pipeline, &link_tag[0], 4);

    ESP_LOGI(TAG, "[2.6] Set up  uri (http as http_stream, mp3 as mp3 decoder, and default output is i2s)");
    audio_element_set_uri(http_stream_reader, "http://stream5.jungletrain.net:8000/");

    ESP_LOGI(TAG, "[ 3 ] Start and wait for Wi-Fi network");

    // Example of using an audio event -- START
    ESP_LOGI(TAG, "[ 4 ] Set up  event listener");
    audio_event_iface_cfg_t evt_cfg = AUDIO_EVENT_IFACE_DEFAULT_CFG();
    audio_event_iface_handle_t evt = audio_event_iface_init(&evt_cfg);

    ESP_LOGI(TAG, "[4.1] Listening event from all elements of pipeline");
    audio_pipeline_set_listener(pipeline, evt);

    // ESP_LOGI(TAG, "[4.2] Listening event from peripherals");
    // esp_periph_config_t periph_cfg = DEFAULT_ESP_PERIPH_SET_CONFIG();
    // esp_periph_set_handle_t set = esp_periph_set_init(&periph_cfg);
    // audio_event_iface_set_listener(esp_periph_set_get_event_iface(set), evt);

    ESP_LOGI(TAG, "[ 5 ] Start audio_pipeline");
    audio_pipeline_run(pipeline);
    while (1)
    {

        audio_event_iface_msg_t msg;
        ESP_LOGI(TAG, "[ * ] Message source type: %i", msg.source_type);
        esp_err_t ret = audio_event_iface_listen(evt, &msg, 10);
        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "[ * ] Event interface error : %d", ret);
            continue;
        }

        if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT && msg.source == (void *)mp3_decoder && msg.cmd == AEL_MSG_CMD_REPORT_MUSIC_INFO)
        {
            audio_element_info_t music_info = {0};
            audio_element_getinfo(mp3_decoder, &music_info);

            ESP_LOGI(TAG, "[ * ] Receive music info from mp3 decoder, sample_rates=%d, bits=%d, ch=%d",
                     music_info.sample_rates, music_info.bits, music_info.channels);

            audio_element_setinfo(i2s_stream_writer, &music_info);
            alc_volume_setup_set_channel(alc_el, music_info.channels);
            i2s_stream_set_clk(i2s_stream_writer, music_info.sample_rates, music_info.bits, music_info.channels);
            continue;
        }

        /* Stop when the last pipeline element (i2s_stream_writer in this case) receives stop event */
        if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT && msg.source == (void *)i2s_stream_writer && msg.cmd == AEL_MSG_CMD_REPORT_STATUS && (((int)msg.data == AEL_STATUS_STATE_STOPPED) || ((int)msg.data == AEL_STATUS_STATE_FINISHED)))
        {
            ESP_LOGW(TAG, "[ * ] Stop event received");
            break;
        }
    }

    // Example of using an audio event -- END

    ESP_LOGI(TAG, "[ 6 ] Stop audio_pipeline");
    audio_pipeline_stop(pipeline);
    audio_pipeline_wait_for_stop(pipeline);
    audio_pipeline_terminate(pipeline);

    /* Terminate the pipeline before removing the listener */
    audio_pipeline_unregister(pipeline, http_stream_reader);
    audio_pipeline_unregister(pipeline, i2s_stream_writer);
    audio_pipeline_unregister(pipeline, mp3_decoder);

    audio_pipeline_remove_listener(pipeline);

    /* Stop all peripherals before removing the listener */
    // esp_periph_set_stop_all(set);
    // audio_event_iface_remove_listener(esp_periph_set_get_event_iface(set), evt);

    /* Make sure audio_pipeline_remove_listener & audio_event_iface_remove_listener are called before destroying event_iface */
    audio_event_iface_destroy(evt);

    /* Release all resources */
    audio_pipeline_deinit(pipeline);
    audio_element_deinit(http_stream_reader);
    audio_element_deinit(i2s_stream_writer);
    audio_element_deinit(mp3_decoder);
    // esp_periph_set_destroy(set);
    return NULL;
}
