#include "ssd1306_lib.h"


int ssd1306_i2c_send(struct ssd1306_i2c_module *module,
                     char *buff,
                     int len)
{
    struct i2c_msg msg;

    msg.addr  = dev_read_addr(module->dev);
    msg.flags = 0;
    msg.len   = len;
    msg.buf   = buff;

    return dm_i2c_xfer(module->dev, &msg, 1);
}

void ssd1306_write(struct ssd1306_i2c_module *module, bool check, char data)
{
    char buff[2] = {0};

    if (check == true){
        buff[0] = 0x00;  // byte tiếp theo là một lệnh
    }
    else{
        buff[0] = 0x40;  // byte tiếp theo là dữ liệu.
    }

    buff[1] = data;
    ssd1306_i2c_send(module, buff, 2);
}

void ssd1306_set_cursor(struct ssd1306_i2c_module *module, uint8_t line_num, uint8_t cursor_position)
{
	if ((line_num <= SSD1306_MAX_LINE) && (cursor_position < SSD1306_MAX_SEG)) {
		module->line_num = line_num;			   // Save the specified line number
		module->cursor_position = cursor_position; // Save the specified cursor position
		ssd1306_write(module, true, 0x21);				   // cmd for the column start and end address
		ssd1306_write(module, true, cursor_position);	   // column start addr
		ssd1306_write(module, true, SSD1306_MAX_SEG - 1);  // column end addr
		ssd1306_write(module, true, 0x22);				   // cmd for the page start and end address
		ssd1306_write(module, true, line_num);			   // page start addr
		ssd1306_write(module, true, SSD1306_MAX_LINE);	   // page end addr
	}
}

void  ssd1306_goto_next_line(struct ssd1306_i2c_module *module){
	module->line_num ++;
	if (module->line_num == SSD1306_MAX_LINE)
	{
        module->line_num = 0;
	}
	ssd1306_set_cursor(module, module->line_num, 0);

}

int convert(char c) {
    return ((int)c - 32);  
}

void ssd1306_print_char(struct ssd1306_i2c_module *module, unsigned char c)
{
	uint8_t data_byte;
	uint8_t temp = 0;
	int pos_line = 0;

	if (((module->cursor_position + module->font_size) >= SSD1306_MAX_SEG) || (c == '\n'))
		ssd1306_goto_next_line(module);

	if (c != '\n') {
		pos_line = convert(c);
		do{
			data_byte = ssd1306_font[pos_line][temp];
            ssd1306_write(module, false , data_byte);
			module->cursor_position++;
			temp ++;
		}while(temp < module->font_size);

		ssd1306_write(module, false, 0x00); 
		module->cursor_position++;
   
	}
}

void ssd1306_print_string(struct ssd1306_i2c_module *module, unsigned char *str){
	while(*str)
	{
		ssd1306_print_char(module, *str ++);
	}
}

void ssd1306_set_brightness(struct ssd1306_i2c_module *module, uint8_t brightness)
{
	ssd1306_write(module, true, 0x81);  // chỉ ra rằng dữ liệu tiếp theo là một lệnh, không phải giá trị pixel. 0x81 là mã lệnh cho việc thiết lập độ sáng (contrast) cho màn hình SSD1306. Điều này báo hiệu cho màn hình rằng byte tiếp theo sẽ là giá trị độ sáng mới cần thiết lập.
    ssd1306_write(module, true, brightness); // độ sáng cần thiết lập cho màn hình ( từ 0 - 255 );
}

void ssd1306_clear_page(struct ssd1306_i2c_module *module, uint8_t line)
{
	ssd1306_set_cursor(module, line, 0);
	int i;
	for ( i = 0; i< 128 ; i++)
	{
		ssd1306_write(module, false, 0);
		//module->cursor_position++;
	}

}

void ssd1306_clear_full(struct ssd1306_i2c_module *module)
{
	uint8_t i;
	for (i = 0; i< 8; i++){
		ssd1306_clear_page(module, i);
	}
}


int ssd1306_display_init(struct ssd1306_i2c_module *module)
{
	msleep(100);
   
	ssd1306_write(module, true, 0xAE); // Entire Display OFF
    ssd1306_write(module, true, 0xA8); // Set Multiplex Ratio
    ssd1306_write(module, true, 0x3F); // 64 COM lines
    ssd1306_write(module, true, 0xD3); // Set display offset
    ssd1306_write(module, true, 0x00); // 0 offset
    ssd1306_write(module, true, 0x40); // Set first line as the start line of the display
    ssd1306_write(module, true, 0xA1); // Set segment remap with column address 127 mapped to segment 0
    ssd1306_write(module, true, 0xC8); // Set com output scan direction, scan from com63 to com 0
    ssd1306_write(module, true, 0xDA); // Set com pins hardware configuration
    ssd1306_write(module, true, 0x12); // Alternative com pin configuration, disable com left/right remap
    ssd1306_write(module, true, 0x81); // Set contrast control
    ssd1306_write(module, true, 0x7F); // Set Contrast to 128
    ssd1306_write(module, true, 0xA4); // Entire display ON, resume to RAM content display
    ssd1306_write(module, true, 0xA6); // Set Display in Normal Mode, 1 = ON, 0 = OFF
    ssd1306_write(module, true, 0xD5); // Set Display Clock Divide Ratio and Oscillator Frequency
    ssd1306_write(module, true, 0x80); // Default Setting for Display Clock Divide Ratio and Oscillator Frequency that is recommended
    ssd1306_write(module, true, 0x8D); //  Charge pump
    ssd1306_write(module, true, 0x14); // Enable charge dump during display on
	ssd1306_write(module, true, 0xAF); // Display ON in normal mode
	ssd1306_clear_full(module);

	return 0;
}
