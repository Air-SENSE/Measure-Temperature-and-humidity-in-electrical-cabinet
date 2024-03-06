
/** Put this in the src folder **/

#include "lcd.h"
#include "esp_log.h"
#include "unistd.h"
#include "string.h"


#define I2C_FREQ_HZ 100000

#define CHECK_ARG(VAL) do { if (!(VAL)) return ESP_ERR_INVALID_ARG; } while (0)

esp_err_t lcd_send_cmd (i2c_dev_t *dev, char cmd)
{
	CHECK_ARG(dev);

	char data_u, data_l;
	uint8_t data_t[4];
	data_u = (cmd & 0xF0);
	data_l = ((cmd << 4) & 0xF0);
	data_t[0] = data_u | 0x0C;  //en=1, rs=0
	data_t[1] = data_u | 0x08;  //en=0, rs=0
	data_t[2] = data_l | 0x0C;  //en=1, rs=0
	data_t[3] = data_l | 0x08;  //en=0, rs=0

	I2C_DEV_TAKE_MUTEX(dev);
    I2C_DEV_CHECK(dev, i2c_dev_write(dev, NULL, 0, data_t, 4));
    I2C_DEV_GIVE_MUTEX(dev);

    return ESP_OK;
}

esp_err_t lcd_send_data (i2c_dev_t *dev, char data)
{
	CHECK_ARG(dev);

	char data_u, data_l;
	uint8_t data_t[4];
	data_u = (data & 0xF0);
	data_l = ((data << 4) & 0xF0);
	data_t[0] = data_u | 0x0D;  //en=1, rs=0
	data_t[1] = data_u | 0x09;  //en=0, rs=0
	data_t[2] = data_l | 0x0D;  //en=1, rs=0
	data_t[3] = data_l | 0x09;  //en=0, rs=0

	I2C_DEV_TAKE_MUTEX(dev);
    I2C_DEV_CHECK(dev, i2c_dev_write(dev, NULL, 0, data_t, 4));
    I2C_DEV_GIVE_MUTEX(dev);

    return ESP_OK;
}

void lcd_clear(i2c_dev_t *dev)
{
	lcd_send_cmd (dev, 0x01);
	vTaskDelay((TickType_t)(5 / portTICK_PERIOD_MS));
}

esp_err_t lcd_put_cur(i2c_dev_t *dev, int row, int col)
{
    switch (row)
    {
        case 0:
            col |= 0x80;
            break;
        case 1:
            col |= 0xC0;
            break;
    }

    return lcd_send_cmd(dev, col);
}

esp_err_t lcd_init (i2c_dev_t *dev, uint8_t addr, i2c_port_t port, gpio_num_t sda_gpio, gpio_num_t scl_gpio)
{
    CHECK_ARG(dev);
    CHECK_ARG(addr & 0x20);

    dev->port = port;
    dev->addr = addr;
    dev->cfg.sda_io_num = sda_gpio;
    dev->cfg.scl_io_num = scl_gpio;
#if HELPER_TARGET_IS_ESP32
    dev->cfg.master.clk_speed = I2C_FREQ_HZ;
#endif

	i2c_dev_create_mutex(dev);

	// 4 bit initialisation
	vTaskDelay((TickType_t)(100 / portTICK_PERIOD_MS));  // wait for >40ms
	lcd_send_cmd(dev, 0x30);
	vTaskDelay((TickType_t)(10 / portTICK_PERIOD_MS));  // wait for >4.1ms
	lcd_send_cmd(dev, 0x30);
	vTaskDelay((TickType_t)(1 / portTICK_PERIOD_MS));  // wait for >100us
	lcd_send_cmd(dev, 0x30);
	vTaskDelay((TickType_t)(10 / portTICK_PERIOD_MS));
	lcd_send_cmd(dev, 0x20);  // 4bit mode
	vTaskDelay((TickType_t)(10 / portTICK_PERIOD_MS));

  // dislay initialisation
	lcd_send_cmd(dev, 0x28); // Function set --> DL=0 (4 bit mode), N = 1 (2 line display) F = 0 (5x8 characters)
	vTaskDelay((TickType_t)(5 / portTICK_PERIOD_MS));
	lcd_send_cmd(dev, 0x08); //Display on/off control --> D=0,C=0, B=0  ---> display off
	vTaskDelay((TickType_t)(5 / portTICK_PERIOD_MS));
	lcd_send_cmd(dev, 0x01);  // clear display
	vTaskDelay((TickType_t)(10 / portTICK_PERIOD_MS));
	lcd_send_cmd(dev, 0x06); //Entry mode set --> I/D = 1 (increment cursor) & S = 0 (no shift)
	vTaskDelay((TickType_t)(10 / portTICK_PERIOD_MS));
	lcd_send_cmd(dev, 0x0C); //Display on/off control --> D = 1, C and B = 0. (Cursor and blink, last two bits)
	vTaskDelay((TickType_t)(10 / portTICK_PERIOD_MS));
	return ESP_OK;
}

void lcd_send_string (i2c_dev_t *dev, char *str)
{
	while (*str) 
	{
		lcd_send_data (dev, *str++);
		vTaskDelay((TickType_t)(5 / portTICK_PERIOD_MS));
	}
}
