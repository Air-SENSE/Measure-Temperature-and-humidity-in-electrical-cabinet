#ifndef __LCD_H__
#define __LCD_H__

#include <stdbool.h>
#include <i2cdev.h>
#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t lcd_init (i2c_dev_t *dev, uint8_t addr, i2c_port_t port, gpio_num_t sda_gpio, gpio_num_t scl_gpio);   // initialize lcd

esp_err_t lcd_send_cmd (i2c_dev_t *dev, char cmd);  // send command to the lcd

esp_err_t _lcd_send_data (i2c_dev_t *dev, char data);  // send data to the lcd

void lcd_send_string (i2c_dev_t *dev, char *str);  // send string to the lcd

esp_err_t lcd_put_cur(i2c_dev_t *dev, int row, int col);  // put cursor at the entered position row (0 or 1), col (0-15);

void lcd_clear(i2c_dev_t *dev);

#ifdef __cplusplus
}
#endif

/**@}*/

#endif /* __LCD_H__ */