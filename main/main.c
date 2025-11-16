#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/spi_master.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "easy_crc.h"
// ── SPI / GPIO Pin Assignments ─────────────────────────────────────────────
#define LCD_HOST       SPI2_HOST
#define PIN_NUM_BCKL   19     
#define PIN_NUM_CLK    18    
#define PIN_NUM_MOSI   5   
#define PIN_NUM_DC     4     
#define PIN_NUM_RST    2     
#define PIN_NUM_CS     15    
#define BACKLIGHT_ON   1     

#define PARALLEL_LINES 64  
#define LCD_WIDTH      480
#define LCD_HEIGHT     320
#define UART_TX_PIN 12
#define UART_RX_PIN 13
#define BUF_SIZE       (PARALLEL_LINES * LCD_WIDTH * 3)  

#define UART_PORT UART_NUM_2
#define UARTBUF_SIZE 1024
#define UART_TX_PIN 12
#define UART_RX_PIN 13
QueueHandle_t uart_queue;
#define PACKET_SIZE 18
#define START_BYTE 0xA5
static const char *TAG = "UART_RX";

typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
} Color;

#define COLOR_BLACK     (Color){0, 0, 0}
#define COLOR_WHITE     (Color){255, 255, 255}
#define COLOR_RED       (Color){255, 0, 0}
#define COLOR_GREEN     (Color){0, 255, 0}
#define COLOR_BLUE      (Color){0, 0, 255}
#define COLOR_CYAN      (Color){0, 255, 255}
#define COLOR_MAGENTA   (Color){255, 0, 255}
#define COLOR_YELLOW    (Color){255, 255, 0}
#define COLOR_GRAY      (Color){128, 128, 128}
#define COLOR_DARK_RED  (Color){128, 0, 0}

typedef enum {
    FONT_SMALL = 1,
    FONT_MEDIUM = 2,
    FONT_LARGE = 3,
    FONT_XLARGE = 4,
    FONT_XXLARGE=5,
    FONT_XXXLARGE=8

} FontSize;

typedef struct {
    float f1, f2, f3,f4;
} UartData;
QueueHandle_t uart_data_queue;

static const uint8_t digit_font[12][8] = {
    // 0
    {
    0b00111100, 
    0b01100110, 
    0b01101110, 
    0b01110110, 
    0b01100110, 
    0b01100110, 
    0b00111100, 
    0b00000000},
    // 1
    {0b00011000, 0b00111000, 0b00011000, 0b00011000,
     0b00011000, 0b00011000, 0b00111100, 0b00000000},
    // 2
    {0b00111100, 0b01100110, 0b00000110, 0b00011100,
     0b00110000, 0b01100000, 0b01111110, 0b00000000},
    // 3
    {0b00111100, 0b01100110, 0b00000110, 0b00011100,
     0b00000110, 0b01100110, 0b00111100, 0b00000000},
    // 4
    {0b00001100, 0b00011100, 0b00101100, 0b01001100,
     0b01111110, 0b00001100, 0b00011110, 0b00000000},
    // 5
    {0b01111110, 0b01100000, 0b01111110, 0b00000110,
     0b00000110, 0b01100110, 0b00111110, 0b00000000},
    // 6
    {0b00111100, 0b01100110, 0b01100000, 0b01111100,
     0b01100110, 0b01100110, 0b00111100, 0b00000000},
    // 7
    {0b01111110, 0b01100110, 0b00000110, 0b00001100,
     0b00011000, 0b00011000, 0b00011000, 0b00000000},
    // 8
    {0b00111100, 0b01100110, 0b01100110, 0b00111100,
     0b01100110, 0b01100110, 0b00111100, 0b00000000},
    // 9
    {0b00111100, 0b01100110, 0b01100110, 0b00111110,
     0b00000110, 0b01100110, 0b00111100, 0b00000000},

    {0b00000000, 
    0b00000000, 
    0b00000000,
    0b00000000,
    0b00000000,
    0b00011000, 
    0b00011000,
    0b00000000},

    // Empty (10)
    {0b00000000, 0b00000000, 0b00000000, 0b00000000,
     0b00000000, 0b00000000, 0b00000000, 0b00000000},
};
static const uint8_t ili9488_init[] = {
    0x01, 0x80 | 0,           
    0x11, 0x80 | 0,           
    
    // Positive Gamma
    0xE0, 15, 0x00,0x07,0x0F,0x0D,0x1B,0x0A,0x3C,0x78,0x4A,0x07,0x0E,0x09,0x1B,0x1E,0x0F,
    // Negative Gamma
    0xE1, 15, 0x00,0x22,0x24,0x06,0x12,0x07,0x36,0x47,0x47,0x06,0x0A,0x07,0x30,0x37,0x0F,

    0xC0, 2, 0x10,0x10,       // Power Control 1
    0xC1, 1, 0x41,            // Power Control 2
    0xC5, 3, 0x00,0x22,0x80,  // VCOM Control
    0x36, 1, 0x28,            // Memory Access (portrait, RGB)
    0x3A, 1, 0x66,            // Interface Pixel Format: 18-bit (reverted from 0x55)
    0xB0, 1, 0x00,            // Interface Mode
    0xB1, 1, 0xA0,            // Frame Rate
    0xB4, 1, 0x02,            // Display Inversion
    0xB6, 3, 0x02,0x02,0x3B,  // Display Function
    0xF7, 4, 0xA9,0x51,0x2C,0x82, // Adjust Control

    0x29, 0x80 | 0,           // Display ON, delay
    0x00                      // End marker
};

// ── Display State ──────────────────────────────────────────────────────────
typedef struct {
    uint16_t width;
    uint16_t height;
    uint8_t rotation;
    Color text_color;
    Color bg_color;
    FontSize font_size;
    uint8_t brightness;
} DisplayState;

static spi_device_handle_t spi_dev;
static uint8_t* dma_buffer;  
static DisplayState display = {
    .width = LCD_WIDTH,
    .height = LCD_HEIGHT,
    .rotation = 0,
    .text_color = COLOR_WHITE,
    .bg_color = COLOR_BLACK,
    .font_size = FONT_MEDIUM,
    .brightness = 100
};


static void lcd_spi_pre_cb(spi_transaction_t *t);
static void lcd_cmd(uint8_t cmd);
static void lcd_data(const void* data, size_t len);
static void lcd_init_sequence(void);
static void lcd_reset_and_backlight(void);
static void lcd_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1);
static void lcd_fill_color(Color color);
static void lcd_draw_pixel(uint16_t x, uint16_t y, Color color);
static void lcd_draw_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, Color color);
static void lcd_draw_filled_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, Color color);
static void lcd_draw_line(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1, Color color);
static void lcd_draw_circle(uint16_t x0, uint16_t y0, uint16_t r, Color color);
static void lcd_draw_filled_circle(uint16_t x0, uint16_t y0, uint16_t r, Color color);
static void lcd_draw_char(uint16_t x, uint16_t y, char c, FontSize scale, Color fg, Color bg);
static void lcd_draw_string(uint16_t x, uint16_t y, const char *str, FontSize scale, Color fg, Color bg);
static void lcd_draw_number(uint16_t x, uint16_t y, int number, FontSize scale, Color fg, Color bg);
static void lcd_set_rotation(uint8_t rotation);
static void lcd_set_brightness(uint8_t percent);
static void lcd_clear(void);
static void uart_init();
static void uart_rx_task(void *arg);

void uart_init()
{
    const uart_config_t uart_config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_APB,
    };

    uart_driver_install(UART_PORT, UARTBUF_SIZE * 2, 0, 10, &uart_queue, 0);
    uart_param_config(UART_PORT, &uart_config);
    uart_set_pin(UART_PORT, UART_TX_PIN, UART_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
}
void uart_rx_task(void *arg)
{
    uint8_t byte;
    uint8_t buffer[PACKET_SIZE - 1];
    uart_event_t event;

    int index = 0;
    bool sync = false;

    while (true)
    {
        if (xQueueReceive(uart_queue, &event, portMAX_DELAY))
        {
            if (event.type == UART_DATA)
            {
                while (uart_read_bytes(UART_PORT, &byte, 1, 0) == 1)
                {
                    if (!sync)
                    {
                        if (byte == START_BYTE)
                        {
                            index = 0;
                            sync = true;
                        }
                        continue;
                    }

                    buffer[index++] = byte;

                    if (index == PACKET_SIZE - 1)
                    {
                        sync = false;

                        uint8_t received_crc = buffer[16];
                        uint8_t calculated_crc = calculate_cr8x_fast(buffer, 16);

                        if (received_crc == calculated_crc)
                        {
                            float f1, f2, f3,f4;
                            memcpy(&f1, &buffer[0], 4);
                            memcpy(&f2, &buffer[4], 4);
                            memcpy(&f3, &buffer[8], 4);
                            memcpy(&f4, &buffer[12], 4);

                             UartData data = { f1, f2, f3 ,f4};
                             xQueueSend(uart_data_queue, &data, portMAX_DELAY);
                            ESP_LOGI(TAG, "Received: %.2f, %.2f, %.2f, %.2f ", f1, f2, f3,f4);
                        }
                        else
                        {
                            ESP_LOGW(TAG, "CRC FAIL: got 0x%02X, expected 0x%02X", received_crc, calculated_crc);
                        }
                    }
                }
            }
        }
    }
}

static void lcd_spi_pre_cb(spi_transaction_t *t) {
    gpio_set_level(PIN_NUM_DC, (int)t->user);
}

static void lcd_cmd(uint8_t cmd) {
    spi_transaction_t t = {
        .length = 8,
        .tx_buffer = &cmd,
        .user = (void*)0  //command
    };
    spi_device_polling_transmit(spi_dev, &t);
}

static void lcd_data(const void* data, size_t len) {
    if (!len) return;
    spi_transaction_t t = {
        .length = len * 8,
        .tx_buffer = data,
        .user = (void*)1    //data
    };
    spi_device_polling_transmit(spi_dev, &t);
}

static void lcd_init_sequence(void) {
    const uint8_t *p = ili9488_init;
    while (1) {
        uint8_t cmd = *p++;
        if (cmd == 0x00) break;
        uint8_t hdr = *p++;
        bool delay_after = hdr & 0x80;
        uint8_t len = hdr & 0x7F;

        lcd_cmd(cmd);
        if (len) {
            lcd_data(p, len);
            p += len;
        }
        if (delay_after) {
            vTaskDelay(pdMS_TO_TICKS(120));
        }
    }
}
static void lcd_reset_and_backlight(void) {
    gpio_config_t io_conf = {
        .pin_bit_mask = BIT(PIN_NUM_DC) | BIT(PIN_NUM_RST) | BIT(PIN_NUM_BCKL),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io_conf);

    gpio_set_level(PIN_NUM_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(PIN_NUM_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(120));  

    gpio_set_level(PIN_NUM_BCKL, BACKLIGHT_ON);
}

static void lcd_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
    uint8_t buf[4];
    
    lcd_cmd(0x2A);
    buf[0] = x0 >> 8; buf[1] = x0 & 0xFF;
    buf[2] = x1 >> 8; buf[3] = x1 & 0xFF;
    lcd_data(buf, 4);

    lcd_cmd(0x2B);
    buf[0] = y0 >> 8; buf[1] = y0 & 0xFF;
    buf[2] = y1 >> 8; buf[3] = y1 & 0xFF;
    lcd_data(buf, 4);

    lcd_cmd(0x2C);
}

static void draw_hline(uint16_t x, uint16_t y, uint16_t w, Color color) {
    if (y >= LCD_HEIGHT || x >= LCD_WIDTH || w == 0) return;
    uint16_t x_end = x + w - 1;
    if (x_end >= LCD_WIDTH) x_end = LCD_WIDTH - 1;
    uint16_t len = x_end - x + 1;
    uint8_t buffer[len * 3];

    lcd_set_window(x, y, x_end, y);
    for (int i = 0; i < len * 3; i += 3) {
        buffer[i]   = color.r;
        buffer[i+1] = color.g;
        buffer[i+2] = color.b;
    }
    spi_transaction_t t = {
        .length    = len * 24,  
        .tx_buffer = buffer,
        .user      = (void*)1,
    };
    spi_device_polling_transmit(spi_dev, &t);
}

static void draw_vline(uint16_t x, uint16_t y, uint16_t h, Color color) {
    if (x >= LCD_WIDTH || y >= LCD_HEIGHT || h == 0) return;

    uint16_t y_end = y + h - 1;
    if (y_end >= LCD_HEIGHT) y_end = LCD_HEIGHT - 1;
    uint16_t height = y_end - y + 1;

    uint8_t buffer[height * 3];
    for (uint16_t i = 0; i < height; ++i) {
        buffer[i * 3 + 0] = color.r;
        buffer[i * 3 + 1] = color.g;
        buffer[i * 3 + 2] = color.b;
    }

    lcd_set_window(x, y, x, y_end);

    spi_transaction_t t = {
        .length    = height * 24,  
        .tx_buffer = buffer,
        .user      = (void*)1,
    };
    spi_device_polling_transmit(spi_dev, &t);
}

static void draw_line(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1, Color color) {
    if (y0 == y1) {
        draw_hline(x0 < x1 ? x0 : x1, y0, abs(x1 - x0) + 1, color);
        return;
    }
    if (x0 == x1) {
        draw_vline(x0, y0 < y1 ? y0 : y1, abs(y1 - y0) + 1, color);
        return;
    }

    uint16_t min_x = x0 < x1 ? x0 : x1;
    uint16_t max_x = x0 > x1 ? x0 : x1;
    uint16_t min_y = y0 < y1 ? y0 : y1;
    uint16_t max_y = y0 > y1 ? y0 : y1;
    uint16_t width = max_x - min_x + 1;
    uint16_t height = max_y - min_y + 1;

    uint8_t buffer[width * height * 3];
    memset(buffer, 0, sizeof(buffer)); 

    int dx = abs((int)x1 - (int)x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs((int)y1 - (int)y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy, e2;

    while (true) {
        uint16_t px = x0 - min_x;
        uint16_t py = y0 - min_y;
        uint32_t offset = (py * width + px) * 3;
        buffer[offset]     = color.r;
        buffer[offset + 1] = color.g;
        buffer[offset + 2] = color.b;

        if (x0 == x1 && y0 == y1) break;
        e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }

    lcd_set_window(min_x, min_y, max_x, max_y);
    spi_transaction_t t = {
        .length    = width * height * 24,
        .tx_buffer = buffer,
        .user      = (void*)1,
    };
    spi_device_polling_transmit(spi_dev, &t);
}


static void lcd_fill_color(Color color) {
    lcd_set_window(0, 0, display.width - 1, display.height - 1);
    
    size_t pixels_per_chunk = PARALLEL_LINES * display.width;
    for (size_t i = 0; i < pixels_per_chunk * 3; i += 3) {
        dma_buffer[i]   = color.r;
        dma_buffer[i+1] = color.g;
        dma_buffer[i+2] = color.b;
    }

    size_t total_chunks = (display.height + PARALLEL_LINES - 1) / PARALLEL_LINES;
    size_t bytes_per_chunk = PARALLEL_LINES * display.width * 3;

    for (size_t i = 0; i < total_chunks; i++) {
        uint16_t y = i * PARALLEL_LINES;
        uint16_t lines = (y + PARALLEL_LINES <= display.height) ? 
                         PARALLEL_LINES : display.height - y;
        
        spi_transaction_t t = {
            .length = lines * display.width * 24,  
            .tx_buffer = dma_buffer,
            .user = (void*)1
        };
        spi_device_polling_transmit(spi_dev, &t);
    }
}

static void lcd_clear(void) {
    lcd_fill_color(display.bg_color);
}

void lcd_draw_bitmap(const uint8_t *img, int x, int y, int w, int h) {
    lcd_set_window(x, y, x + w - 1, y + h - 1);
    spi_transaction_t t = {
        .length = w * h * 3 * 8,
        .tx_buffer = img,
        .user = (void *)1,
    };
    spi_device_polling_transmit(spi_dev, &t);
}
void lcd_clear_number_area(uint16_t x, uint16_t y, uint8_t char_count, FontSize scale, Color bg) {
    uint16_t width = char_count * 8 * scale;
    uint16_t height = 8 * scale;

    lcd_set_window(x, y, x + width - 1, y + height - 1);

    size_t pixels = width * height;
    for (size_t i = 0; i < pixels * 3; i += 3) {
        dma_buffer[i]     = bg.r;
        dma_buffer[i + 1] = bg.g;
        dma_buffer[i + 2] = bg.b;
    }

    size_t lines_per_chunk = PARALLEL_LINES;
    size_t total_chunks = (height + lines_per_chunk - 1) / lines_per_chunk;

    for (size_t i = 0; i < total_chunks; i++) {
        uint16_t line_offset = i * lines_per_chunk;
        uint16_t chunk_height = (line_offset + lines_per_chunk <= height) ?
                                lines_per_chunk : (height - line_offset);

        spi_transaction_t t = {
            .length = chunk_height * width * 24,  // bits
            .tx_buffer = dma_buffer,
            .user = (void*)1
        };

        spi_device_polling_transmit(spi_dev, &t);
    }
}
void lcd_clear_section(uint16_t x, uint16_t y, uint16_t width, uint16_t height, Color color) {
    lcd_set_window(x, y, x + width - 1, y + height - 1);

    // Fill dma_buffer for one chunk of size PARALLEL_LINES * width pixels
    size_t pixels_per_chunk = PARALLEL_LINES * width;
    for (size_t i = 0; i < pixels_per_chunk * 3; i += 3) {
        dma_buffer[i]   = color.r;
        dma_buffer[i+1] = color.g;
        dma_buffer[i+2] = color.b;
    }

    size_t total_chunks = (height + PARALLEL_LINES - 1) / PARALLEL_LINES;
    size_t bytes_per_chunk = PARALLEL_LINES * width * 3;

    for (size_t i = 0; i < total_chunks; i++) {
        uint16_t current_y = i * PARALLEL_LINES;
        uint16_t lines_to_write = (current_y + PARALLEL_LINES <= height) ?
                                  PARALLEL_LINES : height - current_y;

        spi_transaction_t t = {
            .length = lines_to_write * width * 24,  // bits
            .tx_buffer = dma_buffer,
            .user = (void*)1
        };
        spi_device_polling_transmit(spi_dev, &t);
    }
}

static void lcd_set_brightness(uint8_t percent) {
    if (percent > 100) percent = 100;
    display.brightness = percent;
    gpio_set_level(PIN_NUM_BCKL, percent > 0 ? BACKLIGHT_ON : !BACKLIGHT_ON);
}

static void lcd_set_rotation(uint8_t rotation) {
    if (rotation > 3) rotation = 0;
    display.rotation = rotation;
    
    lcd_cmd(0x36); 
    switch (rotation) {
        case 0:  // Portrait
            lcd_data("\x28", 1);
            display.width = LCD_WIDTH;
            display.height = LCD_HEIGHT;
            break;
        case 1:  // Landscape
            lcd_data("\x68", 1);
            display.width = LCD_HEIGHT;
            display.height = LCD_WIDTH;
            break;
        case 2:  // Portrait (flipped)
            lcd_data("\xE8", 1);
            display.width = LCD_WIDTH;
            display.height = LCD_HEIGHT;
            break;
        case 3:  // Landscape (flipped)
            lcd_data("\xA8", 1);
            display.width = LCD_HEIGHT;
            display.height = LCD_WIDTH;
            break;
    }
}

static void lcd_draw_pixel(uint16_t x, uint16_t y, Color color) {
    if (x >= display.width || y >= display.height) return;
    
    lcd_set_window(x, y, x, y);
    uint8_t pixel_data[3] = {color.r, color.g, color.b};
    lcd_data(pixel_data, 3);
}

static void lcd_draw_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, Color color) {
    // Top line
    draw_line(x, y, x + w - 1, y, color);
    // Bottom line
    draw_line(x, y + h - 1, x + w - 1, y + h - 1, color);
    // Left line
    draw_line(x, y, x, y + h - 1, color);
    // Right line
    draw_line(x + w - 1, y, x + w - 1, y + h - 1, color);
}

static void lcd_draw_filled_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, Color color) {
    if (x >= display.width || y >= display.height) return;
    
    uint16_t x_end = x + w - 1;
    uint16_t y_end = y + h - 1;
    
    if (x_end >= display.width) x_end = display.width - 1;
    if (y_end >= display.height) y_end = display.height - 1;
    
    lcd_set_window(x, y, x_end, y_end);
    
    size_t pixels = w * h;
    uint8_t *buffer = malloc(pixels * 3);
    if (!buffer) return;
    
    for (size_t i = 0; i < pixels * 3; i += 3) {
        buffer[i] = color.r;
        buffer[i+1] = color.g;
        buffer[i+2] = color.b;
    }
    
    lcd_data(buffer, pixels * 3);
    free(buffer);
}


static void lcd_draw_circle(uint16_t x0, uint16_t y0, uint16_t r, Color color) {
    // Midpoint circle algorithm
    int f = 1 - r;
    int ddF_x = 1;
    int ddF_y = -2 * r;
    int x = 0;
    int y = r;
    
    lcd_draw_pixel(x0, y0 + r, color);
    lcd_draw_pixel(x0, y0 - r, color);
    lcd_draw_pixel(x0 + r, y0, color);
    lcd_draw_pixel(x0 - r, y0, color);
    
    while (x < y) {
        if (f >= 0) {
            y--;
            ddF_y += 2;
            f += ddF_y;
        }
        x++;
        ddF_x += 2;
        f += ddF_x;
        
        lcd_draw_pixel(x0 + x, y0 + y, color);
        lcd_draw_pixel(x0 - x, y0 + y, color);
        lcd_draw_pixel(x0 + x, y0 - y, color);
        lcd_draw_pixel(x0 - x, y0 - y, color);
        lcd_draw_pixel(x0 + y, y0 + x, color);
        lcd_draw_pixel(x0 - y, y0 + x, color);
        lcd_draw_pixel(x0 + y, y0 - x, color);
        lcd_draw_pixel(x0 - y, y0 - x, color);
    }
}

static void lcd_draw_filled_circle(uint16_t x0, uint16_t y0, uint16_t r, Color color) {
    // Modified midpoint circle algorithm with horizontal lines
    int f = 1 - r;
    int ddF_x = 1;
    int ddF_y = -2 * r;
    int x = 0;
    int y = r;
    
    draw_line(x0, y0 + r, x0, y0 - r, color);
    draw_line(x0 + r, y0, x0 - r, y0, color);
    
    while (x < y) {
        if (f >= 0) {
            y--;
            ddF_y += 2;
            f += ddF_y;
        }
        x++;
        ddF_x += 2;
        f += ddF_x;
        
        draw_line(x0 + x, y0 + y, x0 + x, y0 - y, color);
        draw_line(x0 - x, y0 + y, x0 - x, y0 - y, color);
        draw_line(x0 + y, y0 + x, x0 + y, y0 - x, color);
        draw_line(x0 - y, y0 + x, x0 - y, y0 - x, color);
    }
}

static void lcd_draw_char(uint16_t x, uint16_t y, char c, FontSize scale, Color fg, Color bg) {
    uint8_t digit=0;
if (c >= '0' && c <= '9') {
    digit = c - '0';
} 
else if (c == '.') {
    digit = 10;
}
else if (c == ' ') {
    digit = 11;
} 
else {
    ESP_LOGI("DRAW", "Unexpected char: 0x%02X", c);
    digit = 11;  // fallback to empty
}

    uint16_t width = 8 * scale;
    uint16_t height = 8 * scale;
    
    lcd_set_window(x, y, x + width - 1, y + height - 1);
    
    size_t buf_size = width * height * 3;
    uint8_t *char_buf = malloc(buf_size);
    if (!char_buf) return;
    
    const uint8_t *font = digit_font[digit];
    for (int row = 0; row < 8; row++) {
        uint8_t font_row = font[row];
        for (int col = 0; col < 8; col++) {
            bool pixel_on = (font_row & (1 << (7 - col))) != 0;
            for (int sy = 0; sy < scale; sy++) {
                for (int sx = 0; sx < scale; sx++) {
                    int idx = ((row*scale + sy)*width + (col*scale + sx)) * 3;
                    if (pixel_on) {
                        char_buf[idx]   = fg.r;
                        char_buf[idx+1] = fg.g;
                        char_buf[idx+2] = fg.b;
                    } else {
                        char_buf[idx]   = bg.r;
                        char_buf[idx+1] = bg.g;
                        char_buf[idx+2] = bg.b;
                    }
                }
            }
        }
    }
    
        spi_transaction_t t = {
        .length = buf_size * 8,
        .tx_buffer = char_buf,
        .user = (void*)1    //data
    };
    spi_device_polling_transmit(spi_dev, &t);
    free(char_buf);
}

static void lcd_draw_string(uint16_t x, uint16_t y, const char *str, FontSize scale, Color fg, Color bg) {
    uint16_t char_width = 8 * scale;
    uint16_t char_spacing = 2 * scale;
    
    while (*str) {
        lcd_draw_char(x, y, *str, scale, fg, bg);
        x += char_width + char_spacing;
        str++;
    }
}

void lcd_draw_number_fixed(float value, uint16_t x, uint16_t y, uint8_t length, FontSize scale, Color fg, Color bg) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%.2f", value);  // Convert number to string with 2 decimal places

    size_t actual_len = strlen(buf);
    int padding = (length > actual_len) ? (length - actual_len) : 0;

    for (int i = 0; i < padding; i++) {
        lcd_draw_char(x + i * 8 * scale, y, ' ', scale, fg, bg);
    }

    for (int i = 0; i < actual_len && i + padding < length; i++) {
        lcd_draw_char(x + (i + padding) * 8 * scale, y, buf[i], scale, fg, bg);
    }
}

void lcd_draw_numberf(uint16_t x, uint16_t y, float num, FontSize scale, Color fg, Color bg) {
    char buf[16];  // Enough for large float + 2 decimals
    snprintf(buf, sizeof(buf), "%.2f", num);  // Format with 2 decimal places
    
    for (int i = 0; buf[i]; i++) {
        lcd_draw_char(x + i * (8 * scale), y, buf[i], scale, fg, bg);
    }
}
void app_main(void) {
    ESP_LOGI("LCD", "Initializing display");
    
    lcd_reset_and_backlight();

    dma_buffer = heap_caps_malloc(BUF_SIZE, MALLOC_CAP_DMA);
    if (!dma_buffer) {
        ESP_LOGE("LCD", "DMA buffer allocation failed!");
        return;
    }

    spi_bus_config_t buscfg = {
        .mosi_io_num = PIN_NUM_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = PIN_NUM_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = BUF_SIZE,
        .flags = SPICOMMON_BUSFLAG_MASTER | SPICOMMON_BUSFLAG_GPIO_PINS
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));

    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = 60 * 1000 * 1000,  
        .mode = 0,
        .spics_io_num = PIN_NUM_CS,
        .queue_size = 7,
        .pre_cb = lcd_spi_pre_cb,
        .flags = SPI_DEVICE_HALFDUPLEX
    };
    ESP_ERROR_CHECK(spi_bus_add_device(LCD_HOST, &devcfg, &spi_dev));
    uart_data_queue = xQueueCreate(10, sizeof(UartData)); // holds 10 packets

    if (!uart_data_queue) {
        ESP_LOGE("MAIN", "Failed to create uart_data_queue");
        return;
    }
    lcd_init_sequence();
    
    lcd_set_rotation(0);
    lcd_set_brightness(100);
    lcd_clear();
    uart_init(); 
    xTaskCreatePinnedToCore(uart_rx_task, "uart_rx_task", 4096, NULL, 10, NULL, 1);
    while (1) {
        UartData data;
        if (xQueueReceive(uart_data_queue, &data, pdMS_TO_TICKS(100))) {
            ESP_LOGI("MAIN", "Got data: %.2f, %.2f, %.2f, %.2f", data.f1, data.f2, data.f3,data.f4);
            lcd_draw_number_fixed(data.f1,0,0,6,FONT_XXXLARGE,COLOR_BLUE,COLOR_BLACK);
            lcd_draw_number_fixed(data.f2,10,FONT_XXXLARGE*8,6,FONT_XXXLARGE,COLOR_BLUE,COLOR_BLACK);
            lcd_draw_number_fixed(data.f3,10,FONT_XXXLARGE*8*2,6,FONT_XXXLARGE,COLOR_BLUE,COLOR_BLACK);
            lcd_draw_number_fixed(data.f4,10,FONT_XXXLARGE*8*3,6,FONT_XXXLARGE,COLOR_BLUE,COLOR_BLACK);

        } else {
            ESP_LOGW("MAIN", "Timeout waiting for UART data");
        }
    }
}
