/**
 *******************************************************************************************************************************************
 * @file        lcd_st7789.c
 * @brief       LCD COG [ST7789] driver
 * @since       Change Logs:
 * Date         Author       Notes
 * 2024-02-18   tw           the first version
 * 2024-08-18   lzh          refactor [XXX_WR_DATA]
 *******************************************************************************************************************************************
 * @attention   THE PRESENT FIRMWARE WHICH IS FOR GUIDANCE ONLY AIMS AT PROVIDING CUSTOMERS WITH CODING INFORMATION
 * REGARDING THEIR PRODUCTS IN ORDER FOR THEM TO SAVE TIME. AS A RESULT, SYNWIT SHALL NOT BE HELD LIABLE
 * FOR ANY DIRECT, INDIRECT OR CONSEQUENTIAL DAMAGES WITH RESPECT TO ANY CLAIMS ARISING FROM THE CONTENT
 * OF SUCH FIRMWARE AND/OR THE USE MADE BY CUSTOMERS OF THE CODING INFORMATION CONTAINED HEREIN IN CONN-
 * -ECTION WITH THEIR PRODUCTS.
 * @copyright   2012 Synwit Technology
 *******************************************************************************************************************************************
 */
#include <string.h>
#include "dev_lcd_mpu.h"
#include "SWM221.h"
/* 枚举本驱动已适配 Driver_IC(COG) 为 ST7789 的 LCM 模组型号:
 * 需要注意的是：即便使用了相同的 Driver_IC(COG), 但由于 LCM 模组供应商实现方式有差异(比如内部打线),
 * 不同型号的屏在初始化时序中设置参数部分可能会有个别不同, 需要自行向屏幕供应商索取相应参数.
 * MCU 端仅能提供驱动接口支持, 涉及到屏幕端的技术参数请找屏幕供应商, 以屏幕供应商提供的为准!
 */
#define SIMLEWAY24     "SIMLEWAY24"  /**< 2.4 inch 240*320 思迈微 */
#define ZJY240IT008    "ZJY240IT008" /**< 2.4 inch 240*320 中景园 */

#if 1 /* 调试打印 */
#include <stdio.h>
#define ST7789_LOG(...)        printf(__VA_ARGS__)
#else
#define ST7789_LOG(...)
#endif

/* self is caller function parameters(lcd_mpu_driver_t *) */
#define ST7789_DELAY_MS(ms)             lcd_mpu_driver_mdelay(self, ms)
#define ST7789_WR_CMD(cmd)              lcd_mpu_driver_wr_cmd(self, cmd, NULL)
#define ST7789_WR_DATA(data, bytes)     lcd_mpu_driver_wr_data(self, data, bytes)
#define ST7789_RD_DATA(data, bytes)     lcd_mpu_driver_rd_data(self, data, bytes)

#define ST7789_WR_DATA_VA_ARGS(...)     do {                                    \
                                            uint8_t temp[] = {__VA_ARGS__};     \
                                            ST7789_WR_DATA(temp, sizeof(temp)); \
                                        } while (0)

#define LCD_WR_REG		ST7789_WR_CMD
#define delay_ms 		ST7789_DELAY_MS
#define LCD_WR_DATA 	ST7789_WR_DATA_VA_ARGS

/**
 * @brief 上电初始化时序以配置屏幕参数
 */
extern uint32_t init_lcd_cs;
extern volatile uint8_t test_driver_change;
extern uint8_t last_driver;
extern volatile int lcd_init_time;

static void st7789_apply_direction_mode(lcd_mpu_driver_t *self, uint8_t same_direction)
{
	(void)self;
	init_lcd_cs = 1; // LEFT: reference direction
	ST7789_WR_CMD(0x36);
	ST7789_WR_DATA_VA_ARGS(0xe0);
	init_lcd_cs = 2; // RIGHT
	ST7789_WR_CMD(0x36);
	ST7789_WR_DATA_VA_ARGS(same_direction ? 0xe0 : 0xa0);
	init_lcd_cs = 0;
	last_driver = same_direction;
}
																				
void st7789_timseq_init(lcd_mpu_driver_t *self, lcd_mpu_cfg_t *cfg)
{
	if(lcd_init_time==1){
	//	printf("st7789_timseq_init\r\n");
		if (!(cfg && self && self->hw_rst_gpio_set && self->wr_cmd && self->wr_data && self->mdelay)) {
			ST7789_LOG("[%s] parm error: [hw_rst_gpio_set / write_cmd / write_data / mdelay] is invaild!\r\n", __FUNCTION__);
			//for (;;) __NOP();
		}
		//ST7789_DELAY_MS(120); //Wait for the power supply to stabilize

		/* RST */
		self->hw_rst_gpio_set(1); //H
		ST7789_DELAY_MS(1);
		self->hw_rst_gpio_set(0); //L
		ST7789_DELAY_MS(20);
		self->hw_rst_gpio_set(1); //H
		ST7789_DELAY_MS(120);

	#if 0 /************* Start Initial Sequence **********/
		ST7789_WR_CMD(0x11);
		ST7789_DELAY_MS(150);
		/* ST7789 */
		ST7789_WR_CMD(0xB2);     
		ST7789_WR_DATA_VA_ARGS(0x0C);   
		ST7789_WR_DATA_VA_ARGS(0x0C);   
		ST7789_WR_DATA_VA_ARGS(0x00);   
		ST7789_WR_DATA_VA_ARGS(0x33);   
		ST7789_WR_DATA_VA_ARGS(0x33);  

		ST7789_WR_CMD(0xB7);     
		ST7789_WR_DATA_VA_ARGS(0x76);   

		ST7789_WR_CMD(0xBB);     
		ST7789_WR_DATA_VA_ARGS(0x22);   

		ST7789_WR_CMD(0xC0);     
		ST7789_WR_DATA_VA_ARGS(0x2C);   

		ST7789_WR_CMD(0xC2);     
		ST7789_WR_DATA_VA_ARGS(0x01);   

		ST7789_WR_CMD(0xC3);     
		ST7789_WR_DATA_VA_ARGS(0x13);   

		ST7789_WR_CMD(0xC6);     
		ST7789_WR_DATA_VA_ARGS(0x0F);     

		ST7789_WR_CMD(0xD0);     
		ST7789_WR_DATA_VA_ARGS(0xA4);   
		ST7789_WR_DATA_VA_ARGS(0xA1);   

		ST7789_WR_CMD(0xD6);     
		ST7789_WR_DATA_VA_ARGS(0xA1); 

		ST7789_WR_CMD(0xE0);
		ST7789_WR_DATA_VA_ARGS(0xD0);
		ST7789_WR_DATA_VA_ARGS(0x04);
		ST7789_WR_DATA_VA_ARGS(0x0E);
		ST7789_WR_DATA_VA_ARGS(0x15);
		ST7789_WR_DATA_VA_ARGS(0x16);
		ST7789_WR_DATA_VA_ARGS(0x17);
		ST7789_WR_DATA_VA_ARGS(0x3F);
		ST7789_WR_DATA_VA_ARGS(0x54);
		ST7789_WR_DATA_VA_ARGS(0x4E);
		ST7789_WR_DATA_VA_ARGS(0x09);
		ST7789_WR_DATA_VA_ARGS(0x0F);
		ST7789_WR_DATA_VA_ARGS(0x0D);
		ST7789_WR_DATA_VA_ARGS(0x1C);
		ST7789_WR_DATA_VA_ARGS(0x1E);

		ST7789_WR_CMD(0xE1);
		ST7789_WR_DATA_VA_ARGS(0xD0);
		ST7789_WR_DATA_VA_ARGS(0x02);
		ST7789_WR_DATA_VA_ARGS(0x0A);
		ST7789_WR_DATA_VA_ARGS(0x0C);
		ST7789_WR_DATA_VA_ARGS(0x0E);
		ST7789_WR_DATA_VA_ARGS(0x30);
		ST7789_WR_DATA_VA_ARGS(0x3F);
		ST7789_WR_DATA_VA_ARGS(0x54);
		ST7789_WR_DATA_VA_ARGS(0x4E);
		ST7789_WR_DATA_VA_ARGS(0x3E);
		ST7789_WR_DATA_VA_ARGS(0x1C);
		ST7789_WR_DATA_VA_ARGS(0x1A);
		ST7789_WR_DATA_VA_ARGS(0x23);
		ST7789_WR_DATA_VA_ARGS(0x26);
		
		/* bit 7 : 0 (reserve)
		*
		* bit 6 ~ 4 : RGB interface color format
		* ‘101’ = 65K of RGB interface
		* ‘110’ = 262K of RGB interface
		*
		* bit 3 : 0 (reserve)
		*
		* bit 2 ~ 0 : Control interface color format
		* ‘011’ = 12bit/pixel
		* ‘101’ = 16bit/pixel
		* ‘110’ = 18bit/pixel
		* ‘111’ = 16M truncated
		*/
		ST7789_WR_CMD(0x3A);
		ST7789_WR_DATA_VA_ARGS(0x55);

		/* This command is used to recover from display inversion mode
		* 个别屏内部 Memory 写入值与实际显示是反向的, 详询屏幕供应商
		*/
	//    ST7789_WR_CMD(0x21);
		
		ST7789_WR_CMD(0x36);
		/* D3 : RGB/BGR Order
		* D1 ~ 0 : reserve
		*/
		if (0 == strcmp(cfg->name, ZJY240IT008)) {
			ST7789_WR_DATA_VA_ARGS(0x00);
		}
		else if (0 == strcmp(cfg->name, SIMLEWAY24)) {
			ST7789_WR_DATA_VA_ARGS(0x48); /* 有些屏内部接线 R 与 B 是对调的, 有些则不是, 详询屏幕供应商 */
		}

		ST7789_WR_CMD(0x29);
		ST7789_DELAY_MS(120); /* Sometimes it can also be ignored */
	#endif
	#if 0
	if (1) {	//临时
	//lcd初始化时序
		LCD_WR_REG(0x11);
		delay_ms(120);                //Delay 120ms

		LCD_WR_REG(0xB1);
		LCD_WR_DATA(0x05);
		LCD_WR_DATA(0x3C);
		LCD_WR_DATA(0x3C);

		LCD_WR_REG(0xB2);
		LCD_WR_DATA(0x05);
		LCD_WR_DATA(0x3C);
		LCD_WR_DATA(0x3C);

		LCD_WR_REG(0xB3);     
		LCD_WR_DATA(0x05);   
		LCD_WR_DATA(0x3C);   
		LCD_WR_DATA(0x3C);   
		LCD_WR_DATA(0x05);   
		LCD_WR_DATA(0x3C);   
		LCD_WR_DATA(0x3C);   

		LCD_WR_REG(0xB4);     //Dot inversion
		LCD_WR_DATA(0x03);   

		LCD_WR_REG(0xC0);     
		LCD_WR_DATA(0xA4);   
		LCD_WR_DATA(0x04);   
		LCD_WR_DATA(0x84);   

		LCD_WR_REG(0xC1);     
		LCD_WR_DATA(0xC4);   

		LCD_WR_REG(0xC2);     
		LCD_WR_DATA(0x0D);   
		LCD_WR_DATA(0x00);   

		LCD_WR_REG(0xC3);     
		LCD_WR_DATA(0x8D);   
		LCD_WR_DATA(0x2A);   

		LCD_WR_REG(0xC4);     
		LCD_WR_DATA(0x8D);   
		LCD_WR_DATA(0xEE);   

		LCD_WR_REG(0xC5);     
		LCD_WR_DATA(0x04);   

		LCD_WR_REG(0xE0);     
		LCD_WR_DATA(0x0D);   
		LCD_WR_DATA(0x19);   
		LCD_WR_DATA(0x14);   
		LCD_WR_DATA(0x22);   
		LCD_WR_DATA(0x3F);   
		LCD_WR_DATA(0x37);   
		LCD_WR_DATA(0x2E);   
		LCD_WR_DATA(0x30);   
		LCD_WR_DATA(0x2C);   
		LCD_WR_DATA(0x2A);   
		LCD_WR_DATA(0x2F);   
		LCD_WR_DATA(0x39);   
		LCD_WR_DATA(0x00);   
		LCD_WR_DATA(0x04);   
		LCD_WR_DATA(0x01);   
		LCD_WR_DATA(0x10);   

		LCD_WR_REG(0xE1);     
		LCD_WR_DATA(0x0A);   
		LCD_WR_DATA(0x17);   
		LCD_WR_DATA(0x10);   
		LCD_WR_DATA(0x1A);   
		LCD_WR_DATA(0x39);   
		LCD_WR_DATA(0x32);   
		LCD_WR_DATA(0x2C);   
		LCD_WR_DATA(0x2F);   
		LCD_WR_DATA(0x2E);   
		LCD_WR_DATA(0x29);   
		LCD_WR_DATA(0x32);   
		LCD_WR_DATA(0x3D);   
		LCD_WR_DATA(0x00);   
		LCD_WR_DATA(0x03);   
		LCD_WR_DATA(0x03);   
		LCD_WR_DATA(0x10);   

		LCD_WR_REG(0x2A);     
		LCD_WR_DATA(0x00);   
		LCD_WR_DATA(0x00);   
		LCD_WR_DATA(0x00);   
		LCD_WR_DATA(0x7F);   

		LCD_WR_REG(0x2B);     
		LCD_WR_DATA(0x00);   
		LCD_WR_DATA(0x00);   
		LCD_WR_DATA(0x00);   
		LCD_WR_DATA(0x9F);   

		LCD_WR_REG(0x35);     
		LCD_WR_DATA(0x00);   

		LCD_WR_REG(0x3A);     //65k mode
		LCD_WR_DATA(0x05);   
		lcd_init_time = 0;//只会在上电的时候跑如上流程
	}
	if(test_driver_change == 0){
		init_lcd_cs = 1;//right
		LCD_WR_REG(0x36);
		LCD_WR_DATA(0xc8);
		init_lcd_cs = 2;//left
		LCD_WR_REG(0x36);
		LCD_WR_DATA(0x88);  
		init_lcd_cs = 0;
	} else {
		init_lcd_cs = 1;//right
		LCD_WR_REG(0x36);
		LCD_WR_DATA(0x48);   //
		init_lcd_cs = 2;//left
		LCD_WR_REG(0x36);
		LCD_WR_DATA(0x88);  
		init_lcd_cs = 0;
	}

	LCD_WR_REG(0x2C);     
	LCD_WR_REG(0x29);     //Display on
	#endif

	//st7735s driver
	#define ST7735S_CONFIG			1
	#define Orientation_oldcode		1
	#if 1
	#if ST7735S_CONFIG
		ST7789_DELAY_MS(1000);				//RESET 
		ST7789_WR_CMD(0x11);       //Sleep Out
		ST7789_DELAY_MS(120);               //DELAY120ms 
		
		ST7789_WR_CMD(0x36); 		//Memory Data Access Control
		ST7789_WR_DATA_VA_ARGS(0xC0); 			//Y，X反转，RGB模式
	
	//--------------------------------ST7789S Frame rate setting----------------------------------// 
		ST7789_WR_CMD(0xB1); 		//Panel Function set
		ST7789_WR_DATA_VA_ARGS(0x01); 
		ST7789_WR_DATA_VA_ARGS(0x2C); 
		ST7789_WR_DATA_VA_ARGS(0x2D); 

		ST7789_WR_CMD(0xB2); 
		ST7789_WR_DATA_VA_ARGS(0x01); 
		ST7789_WR_DATA_VA_ARGS(0x2C); 
		ST7789_WR_DATA_VA_ARGS(0x2D); 

		ST7789_WR_CMD(0xB3); 
		ST7789_WR_DATA_VA_ARGS(0x01); 
		ST7789_WR_DATA_VA_ARGS(0x2C); 
		ST7789_WR_DATA_VA_ARGS(0x2D); 
		ST7789_WR_DATA_VA_ARGS(0x01); 
		ST7789_WR_DATA_VA_ARGS(0x2C); 
		ST7789_WR_DATA_VA_ARGS(0x2D); 
		
		ST7789_WR_CMD(0xB4); //Column inversion 
		ST7789_WR_DATA_VA_ARGS(0x07); 

		ST7789_WR_CMD(0x20);     //Display Inversion Off
	//ST7789_WR_CMD(0x21);     //Display inversion on
	
	//---------------------------------ST7735S Power setting--------------------------------------// 
		ST7789_WR_CMD(0xC0); 		//LCM Control
		ST7789_WR_DATA_VA_ARGS(0xA2); 
		ST7789_WR_DATA_VA_ARGS(0x02); 
		ST7789_WR_DATA_VA_ARGS(0x84); 
		
		ST7789_WR_CMD(0xC1); 	//Power Control Setting 
		ST7789_WR_DATA_VA_ARGS(0xC5); 

		ST7789_WR_CMD(0xC2); 		//In Normal Mode (Full Colors) 
		ST7789_WR_DATA_VA_ARGS(0x0A); 
		ST7789_WR_DATA_VA_ARGS(0x00); 

		ST7789_WR_CMD(0xC3); 			//In Idle Mode (8-colors)
		ST7789_WR_DATA_VA_ARGS(0x8A); 
		ST7789_WR_DATA_VA_ARGS(0x2A); 
		
		ST7789_WR_CMD(0xC4); 			//In Partial Mode + Full colors 
		ST7789_WR_DATA_VA_ARGS(0x8A); 
		ST7789_WR_DATA_VA_ARGS(0xEE); 
		
		ST7789_WR_CMD(0xC5); //VCOM Control 1 
		ST7789_WR_DATA_VA_ARGS(0x0E);
		lcd_init_time = 0;//只会在上电的时候跑如上流程
	}
	#endif

	//--------------------------------ST7789S gamma setting---------------------------------------// 
#if 1
	ST7789_WR_CMD(0xe0); 
	ST7789_WR_DATA_VA_ARGS(0x0f); 
		ST7789_WR_DATA_VA_ARGS(0x1a); 
		ST7789_WR_DATA_VA_ARGS(0x0f); 
		ST7789_WR_DATA_VA_ARGS(0x18); 
		ST7789_WR_DATA_VA_ARGS(0x2f); 
		ST7789_WR_DATA_VA_ARGS(0x28); 
		ST7789_WR_DATA_VA_ARGS(0x20); 
		ST7789_WR_DATA_VA_ARGS(0x22); 
		ST7789_WR_DATA_VA_ARGS(0x1f); 
		ST7789_WR_DATA_VA_ARGS(0x1b); 
		ST7789_WR_DATA_VA_ARGS(0x23); 
		ST7789_WR_DATA_VA_ARGS(0x37); 
		ST7789_WR_DATA_VA_ARGS(0x00); 	
		ST7789_WR_DATA_VA_ARGS(0x07); 
		ST7789_WR_DATA_VA_ARGS(0x02); 
		ST7789_WR_DATA_VA_ARGS(0x10); 

		ST7789_WR_CMD(0xe1);         //Negative Voltage Gamma Contro
		ST7789_WR_DATA_VA_ARGS(0x0f); 
		ST7789_WR_DATA_VA_ARGS(0x1b); 
		ST7789_WR_DATA_VA_ARGS(0x0f); 
		ST7789_WR_DATA_VA_ARGS(0x17); 
		ST7789_WR_DATA_VA_ARGS(0x33); 
		ST7789_WR_DATA_VA_ARGS(0x2c); 
		ST7789_WR_DATA_VA_ARGS(0x29); 
		ST7789_WR_DATA_VA_ARGS(0x2e); 
		ST7789_WR_DATA_VA_ARGS(0x30); 
		ST7789_WR_DATA_VA_ARGS(0x30); 
		ST7789_WR_DATA_VA_ARGS(0x39); 
		ST7789_WR_DATA_VA_ARGS(0x3f); 
		ST7789_WR_DATA_VA_ARGS(0x00); 
		ST7789_WR_DATA_VA_ARGS(0x07); 
		ST7789_WR_DATA_VA_ARGS(0x03); 
		ST7789_WR_DATA_VA_ARGS(0x10); 
	
		ST7789_WR_CMD(0x2a);				//Column address set
		ST7789_WR_DATA_VA_ARGS(0x00);			//start column
		ST7789_WR_DATA_VA_ARGS(0x00+2);
		ST7789_WR_DATA_VA_ARGS(0x00);			//end column	
		ST7789_WR_DATA_VA_ARGS(0x80+2);

		ST7789_WR_CMD(0x2b);			//Row address set  
		ST7789_WR_DATA_VA_ARGS(0x00);			//start row
		ST7789_WR_DATA_VA_ARGS(0x00+3);
		ST7789_WR_DATA_VA_ARGS(0x00);			//end row
		ST7789_WR_DATA_VA_ARGS(0x80+3);
		
		ST7789_WR_CMD(0xF0); //Enable test command  
		ST7789_WR_DATA_VA_ARGS(0x01); 
		ST7789_WR_CMD(0xF6); //Disable ram power save mode 
		ST7789_WR_DATA_VA_ARGS(0x00); 
		
		ST7789_WR_CMD(0x3A); //Interface Pixel Format  
		ST7789_WR_DATA_VA_ARGS(0x05); 	//16BIT mode	 
#endif
	

	st7789_apply_direction_mode(self, test_driver_change);


	ST7789_WR_CMD(0x29);       //Display on
	ST7789_WR_CMD(0x2C);     //Memory write
	#endif
}

/**
 * @brief  设置显示窗口
 * @param  xs\xe\ys\ye : 窗口区域坐标(以 0 为起点)
 * @retval \
 */
void st7789_set_disp_area(lcd_mpu_driver_t *self, uint16_t xs, uint16_t xe, uint16_t ys, uint16_t ye)
{
	if (test_driver_change != last_driver) {
		st7789_apply_direction_mode(self, test_driver_change);
	}

	      uint16_t tmp = xs; xs = ys; ys = tmp;
        tmp = xe; xe = ye; ye = tmp;
	
	xs+=2;	xe+=2;

	ys+=1;	ye+=1;

	
    ST7789_WR_CMD(0x2A); /* X */
//	    ST7789_WR_CMD(0x2B); /* Y */
    ST7789_WR_DATA_VA_ARGS((xs >> 8) & 0xFF, xs & 0xFF, 
                           (xe >> 8) & 0xFF, xe & 0xFF);

    ST7789_WR_CMD(0x2B); /* Y */
//		ST7789_WR_CMD(0x2A); /* X */
    ST7789_WR_DATA_VA_ARGS((ys >> 8) & 0xFF, ys & 0xFF, 
                           (ye >> 8) & 0xFF, ye & 0xFF);

    ST7789_WR_CMD(0x2C); /* display on */
		//printf("st7789_set_disp_area\r\n");
}

/**
 * @brief  设置屏幕旋转
 * @param  direction : 旋转方向(自定义实现)
 * @retval \
 */
void st7789_set_rotate(lcd_mpu_driver_t *self, uint8_t direction)
{
	printf("st7789_set_rotate\r\n");
    ST7789_WR_CMD(0x36);
    /* Memory Access Control (36h)
     * This command defines read/write scanning direction of the frame memory.
     *
     * These 3 bits control the direction from the MPU to memory write/read.
     *
     * Bit  Symbol  Name  Description
     * D7   MY  Row Address Order     -- 以X轴镜像
     * D6   MX  Column Address Order  -- 以Y轴镜像
     * D5   MV  Row/Column Exchange   -- X轴与Y轴交换
     * D4   ML  Vertical Refresh Order  LCD vertical refresh direction control.
     *
     * D3   BGR RGB-BGR Order   Color selector switch control
     *      (0 = RGB color filter panel, 1 = BGR color filter panel )
     * D2   MH  Horizontal Refresh ORDER  LCD horizontal refreshing direction control.
     * D1   X   Reserved  Reserved
     * D0   X   Reserved  Reserved
     */
    uint8_t h4 = 0x04, l4 = 0x08; /* 有些屏内部接线 R 与 B 是对调的, 有些则不是, 详询屏幕供应商 */
//    switch (direction)
//    {
//    default : //break;
//    case 0:
//        h4 = 0;
//        break;
//    case 1:
//        h4 = 1 << 1;
//        break;
//    case 2:
//        h4 = 1 << 2;
//        break;
//    case 3:
//        h4 = 1 << 3;
//        break;
//    
//    case 4:
//        h4 = (1 << 1) | (1 << 2);
//        break;
//    case 5:
//        h4 = (1 << 1) | (1 << 3);
//        break;
//    case 6:
//        h4 = (1 << 2) | (1 << 3);
//        break;
//    case 7:
//        h4 = (1 << 1) | (1 << 2) | (1 << 3);
//        break;
//    }
//    ST7789_WR_DATA_VA_ARGS((h4 << 4) | l4);
		
		    ST7789_WR_DATA_VA_ARGS(0x08);

}

/**
 * @brief  设置屏幕背光
 * @param  val : 期望设置亮度
 * @retval 实际设置亮度
 */
uint32_t st7789_set_backlight(lcd_mpu_driver_t *self, uint32_t val)
{
    /* 一些 COG 也支持以发送指令的方式来调节亮度,
    * 亦可直接使用 GPIO 控制 TFT-LCD 的 LED-K 或 使用硬件 PWM 模块实现高频 PWM 调光
    */
    return 0;
}

/**
 * @brief  设置屏幕电源
 * @param  mode : 0-低功耗  other-正常
 * @retval \
 */

void st7789_set_power(lcd_mpu_driver_t *self, uint8_t mode)
{
    /* 一些 COG 也支持以发送指令的方式来进入低功耗模式,
     * 亦可直接使用 GPIO 控制 TFT-LCD 的 Power In
     */
	if(mode==0)
	{
//		printf("SLEEP\r\n");
		GPIO_ClrBit(GPIOB, PIN6);
		ST7789_DELAY_MS(120); 
		ST7789_WR_CMD(0x10);
		ST7789_DELAY_MS(120); 
		
	}		
	else if(mode==1)
	{
//		printf("WAKE_UP\r\n");
		ST7789_WR_CMD(0x11);
		ST7789_DELAY_MS(120); 
//		ST7789_DELAY_MS(120);
		GPIO_SetBit(GPIOB, PIN6);
	}
	else if(mode==2)
	{
		ST7789_WR_CMD(0x11);
		ST7789_DELAY_MS(120); 
	}
	
}
