#include "sc7a20h_driver.h"
#include "SWM221.h"
#include "board.h"
#include "sc7a20h_user.h"


#include <math.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

// ----------------- Define -----------------
#define SL_SC7A20H_SPI_EN_I2C_DISABLE	0x00	//0x01驱动使能SPI方式,失能I2C方式;反之
#define SL_SC7A20H_RAWDATA_HPF_ENABLE	0x00
#define SL_SC7A20H_INT_DEFAULT_LEVEL	0x01
#define SL_SC7A20H_FIFO_ENABLE			0x00	//0x00-FIFO-DISABLE  0x01-FIFO ENABLE

// 寄存器地址
#define WHO_AM_I_REG     	0x0F
#define SC7A20H_VERSION		0x70
#define CTRL_REG1        	0x20		//加速度计的输出频率
/**
 * CTRL_REG1:	量程配置(灵敏度注意要适配量程),精度选择
 *  bit7   bit6   bit5   bit4   bit3   bit2   bit1   bit0
 *  ODR3   ODR2	  ODR1   ODR0	LPen   Zen	   Yen	 Xen
 * [ODR3-ODR0:  输出频率,查表]
 * LPen:	低功耗模式相关
 * Zen:		Z轴使能
 * Yen:		Y轴使能
 * Xen:		X轴使能
 * 0x57		0101 0111		100Hz
 */
#define CTRL_REG2			0x21		//加速度计内置高通滤波器 HPF
/**
 * CTRL_REG2:	内置高通滤波器配置
 *  bit7   bit6   bit5   bit4   bit3   bit2   bit1   bit0
 *  HPM1   HPM0   HPCF2  HPCF1  FDS   HPCLICK HPIS2  HPIS1
 * 
 *  HPM1,HPM0:   设置高通滤波模式
 *  HPCF2,HPCF1: 设置高通滤波器截至频率
 *  FDS:		 滤波数据的选择(是否使用高通滤波数据,0:不使用,1:使用)
 *  HPIS2: 中断发生器AOI2
 *  HPIS1: 中断发生器AOI1
*/
#define CTRL_REG4        	0x23
/**
 * CTRL_REG4:	量程配置(灵敏度注意要适配量程),精度选择
 *  bit7   bit6   bit5   bit4   bit3   bit2   bit1   bit0
 *  BDU    BLE   FS1    FS0    HR     ST1    ST0    SIM
 * 
 * CTRL_REG4 bit[3] HR寄存器和 CTRL_REG1 bit[3] LPen寄存器 共同控制工作模式[低功耗/正常工作模式]
 */
#define CTRL_REG6			0x25		//中断配置相关

#define CTRL_REG5_A			0x24		//FIFO

#define OUT_X_L_REG      	0x28		//sc7a20h各轴读取寄存器
#define OUT_X_H_REG      	0x29
#define OUT_Y_L_REG      	0x2A
#define OUT_Y_H_REG      	0x2B
#define OUT_Z_L_REG      	0x2C
#define OUT_Z_H_REG      	0x2D

//寄存器0xA8 官方驱动里的数据读取寄存器,不可用,手册没有,使用手册读取方式
//注意:当前购买模块,判断长边为Y,短边为X，Z轴垂直芯片朝上.(数值坐标系中,左为+,前为+,上为+,水平静置默认Z=+1g. 
//加速度计: 旋转得世界坐标系一指向传感器坐标系中心O的重力,在传感器坐标系O-X-Y-Z的3轴的投影.

// ----------------- Personal -----------------


// ------------------ Groble ------------------
// char mst_txbuff[4] = {0x37, 0x55, 0xAA, 0x78};
char mst_txbuff[1] = {0x01};
char mst_rxbuff[4] = {0};
uint32_t SLV_ADDR = 0x19;			//i2c salve addr (Default to 0x19 per schematic SDO=H)


// ----------------- Personal -----------------
// sacn i2c bus to get sc7a20h addr.
static err_t i2c_scan(void)
{
    uint8_t ack;
    printf("Scanning I2C bus...\r\n");

    for (uint8_t addr = 1; addr < 127; addr++) {
        ack = I2C_Start(I2C0, (addr << 1) | 0, 1);  // 写模式发起 START + 地址
        if(ack == 1) {
            I2C_Stop(I2C0, 1);
            printf("Device found at 0x%02X\r\n", addr);
            printf("Scan complete.\r\n");
            SLV_ADDR = addr;
			return SWM_OK;
        }
        I2C_Stop(I2C0, 1);
    }
    printf("Scan failed.\r\n");
    return SWM_FAIL;
}

// i2c0 driver (SCL-PB4 SDA-PB5).读取
static void Board_i2c_Init(void)
{
	I2C_InitStructure I2C_initStruct;
	
	PORT_Init(PORTB, PIN4, PORTB_PIN4_I2C0_SCL, 1);		//GPIOB.4配置为I2C0 SCL引脚
	PORTB->OPEND |= (1 << PIN4);						//开漏输出
	PORTB->PULLU |= (1 << PIN4);						//使能上拉
	PORT_Init(PORTB, PIN5, PORTB_PIN5_I2C0_SDA, 1);		//GPIOB.5配置为I2C0 SDA引脚
	PORTB->OPEND |= (1 << PIN5);						//开漏输出
	PORTB->PULLU |= (1 << PIN5);						//使能上拉
	
	I2C_initStruct.Master = 1;                          //1 主机模式    0 从机模式
	I2C_initStruct.MstClk = 100000;                     //主机传输时钟频率
	I2C_initStruct.Addr10b = 0;                         //1 10位地址模式     0 7位地址模式
	I2C_initStruct.TXEmptyIEn = 0;                      //发送寄存器空中断使能
	I2C_initStruct.RXNotEmptyIEn = 0;                   //接收寄存器非空中断使能
	I2C_Init(I2C0, &I2C_initStruct);
	
	I2C_Open(I2C0);
}

// write 1 Byte "data" to sc7a20h "reg" register.
static err_t Sc7a20h_Write_Byte(uint8_t reg, uint8_t data)
{
	// printf("write\r\n");
	//写模式:	(SLV_ADDR << 1)|0
    uint8_t ack = I2C_Start(I2C0, (SLV_ADDR << 1) | 0, 1);
    if(ack == 0) {
  		printf("%s:%d\r\n", __func__, __LINE__);
        goto nextloop;
    }
    ack = I2C_Write(I2C0, reg, 1);		//写入的寄存器
    if(ack == 0) {
  		printf("%s:%d\r\n", __func__, __LINE__);
        goto nextloop;
    }
	ack = I2C_Write(I2C0, data, 1);		//该寄存器写入的数据
    if(ack == 0) {
  		printf("%s:%d\r\n", __func__, __LINE__);
        goto nextloop;
    }
	I2C_Stop(I2C0, 1);
	return SWM_OK;
	
nextloop:
        I2C_Stop(I2C0, 1);
		return SWM_FAIL;
}

// read "buf" from sc7a20h "reg" register.
static err_t Sc7a20h_Read_Bytes(uint8_t reg, uint8_t *buf, size_t len)
{
	// printf("read\r\n");
	//写模式:	(SLV_ADDR << 1)|0
	uint8_t ack = I2C_Start(I2C0, (SLV_ADDR << 1) | 0, 1);
	if(len != 1) {											//该函数仅适用于len = 1
		// printf("%s:%d\r\n", __func__, __LINE__);
		goto nextloop;
	}
	if(ack == 0) {
  		// printf("%s:%d\r\n", __func__, __LINE__);
        goto nextloop;
    }
	I2C0->TR = (0x01 << I2C_TR_TXACK_Pos);
    ack = I2C_Write(I2C0, reg, 1);							//读取的寄存器
	I2C0->TR = (0x01 << I2C_TR_TXACK_Pos);
    if(ack == 0) {
  		printf("%s:%d\r\n", __func__, __LINE__);
        goto nextloop;
    }
	//读模式:	(SLV_ADDR << 1)|1
	ack = I2C_Start(I2C0, (SLV_ADDR << 1) | 1, 1);
    if(ack == 0) {
        // printf("%s:%d\r\n", __func__, __LINE__);
        goto nextloop;
    }
	// printf("%d,%d,%d\r\n",sizeof(buf),sizeof(*buf),len);	//元素，指针
	// I2C0->TR &= ~(0x01 << I2C_TR_TXACK_Pos);				//不使用,I2C_Read(I2Cx,0,1)自带
	// 【注意】,最后一个i2c_read要反馈NACK,非最后一个反馈ACK
	(*buf) = I2C_Read(I2C0, 0, 1);							//save buf
	/*
		完整I2C Read流程:
		START
		→ (0x19 << 1) | 0  // 写模式			I2C_Start(I2C0, (SLV_ADDR << 1) | 0, 1);
		→ ACK
		→ 0x0F             // 要读取的寄存器地址
		→ ACK
		→ RESTART
		→ (0x19 << 1) | 1  	// 读模式			I2C_Start(I2C0, (SLV_ADDR << 1) | 0, 1);
		→ ACK									
		→ 读取数据 (0x28)						I2C_Read(I2C0, 0, 1)
		→ NACK									
		→ STOP									I2C_Stop()
	*/
	// printf("result:0x%x,bus busy? %x\r\n",*buf,((I2C0->SR & I2C_SR_BUSY_Msk)? 1 : 0));
    I2C_Stop(I2C0, 1);										//必不可少, 官方demo缺少, 未验证其结果
	return SWM_OK;

nextloop:
        I2C_Stop(I2C0, 1);
		return SWM_FAIL;
}

// check sc7a20h version.
static err_t SL_Sc7a20h_Check(void)
{
	unsigned char reg_value1=0;
	unsigned char reg_value2=0;
	
	uint8_t who_am_i=0xff;
    err_t err = Sc7a20h_Read_Bytes(WHO_AM_I_REG, &who_am_i, 1);
	printf("who am i:0x%x\r\n",who_am_i);
    if (err != SWM_OK) {
        printf("read WHO_AM_I register failed: 0x%x\r\n", err);
        return err;
    }

	uint8_t version=0xff;
    err = Sc7a20h_Read_Bytes(SC7A20H_VERSION, &version, 1);
    printf("version:0x%x\r\n",version);
	if (err != SWM_OK) {
        printf("read SC7A20H_VERSION register failed: 0x%x\r\n", err);
        return err;
    }
	printf("successful i2c communication.\r\n");
	if((who_am_i==0x11)&&(version==0x28))
		return SWM_OK;
	else
		return SWM_FAIL;
}

// power sc7a20h down.
static err_t SL_Sc7a20h_Power_Down(void)
{
	err_t err = Sc7a20h_Write_Byte(0x20, 0x00); // POWER DOWN
    // Sc7a20h_Write_Byte(0x20, 0x00);//POWER DOWN
    if (err != SWM_OK) {
        printf("write 0x20 register failed: 0x%x\r\n", err);
        return err;
    }

	uint8_t SL_Read_Reg=0xff;
    err = Sc7a20h_Read_Bytes(0x20, &SL_Read_Reg, 1);
    if (err != SWM_OK) {
        printf("write 0x20 register failed: 0x%x\r\n", err);
        return err;
    }
	if(SL_Read_Reg==0x00)   return  SWM_OK;
	else                    return  SWM_FAIL;
}


// ------------------ Groble ------------------
// i2c write/read demo.
void I2C_Test_Demo(void) {
    uint32_t iee;
    uint8_t ack;
    /*************************** Master Write ************************************/
    #define I2C_MST_WRITE       1
    #if I2C_MST_WRITE
    ack = I2C_Start(I2C0, (SLV_ADDR << 1) | 0, 1);
    if(ack == 0) {
        printf("Slave send NACK for address.\r\n");
        goto nextloop;
    }
    
    // for(iee = 0; iee < 4; iee++) {
    // 	ack = I2C_Write(I2C0, mst_txbuff[iee], 1);
    // 	if(ack == 0) {
    // 		printf("Slave send NACK for data\r\n");
    // 		goto nextloop;
    // 	}
    // }
    ack = I2C_Write(I2C0, mst_txbuff[0], 1);
    if(ack == 0) {
        printf("Slave send NACK for data.\r\n");
        goto nextloop;
    }

    I2C_Stop(I2C0, 1);
    // printf("Master Send %X %X %X %X\r\n", mst_txbuff[0], mst_txbuff[1], mst_txbuff[2], mst_txbuff[3]);
    printf("Master Send %X.\r\n",mst_txbuff[0]);
    #endif
    
    /********************************** Master Read *******************************/		
    #define I2C_MST_READ        0
    #if I2C_MST_READ
    ack = I2C_Start(I2C0, (SLV_ADDR << 1) | 1, 1);
    if(ack == 0) {
        printf("Slave send NACK for address.\r\n");
        goto nextloop;
    }
    for(iee = 0; iee < 3; iee++) {
        mst_rxbuff[iee] = I2C_Read(I2C0, 1, 1);
    }
    mst_rxbuff[iee] = I2C_Read(I2C0, 0, 1);
    printf("Master Read %X %X %X %X\r\n", mst_rxbuff[0], mst_rxbuff[1], mst_rxbuff[2], mst_rxbuff[3]);
    if((mst_txbuff[0] == mst_rxbuff[0]) && (mst_txbuff[1] == mst_rxbuff[1]) && (mst_txbuff[2] == mst_rxbuff[2]) && (mst_txbuff[3] == mst_rxbuff[3]))
        printf("Success\r\n");
    else
        printf("Fail\r\n");
    #endif

nextloop:
        I2C_Stop(I2C0, 1);
        for(iee = 0; iee < SystemCoreClock/8; iee++);
}

// init sc7a20h i2c.
err_t Sc7a20h_i2c_Init()
{
	Board_i2c_Init();
    
	err_t err = i2c_scan();      //get sc7a20h addr
    if (err == SWM_OK) {
		printf("I2C driver success: 0x%x\r\n", err);
    } else {
        printf("I2C driver failed: 0x%x\r\n", err);
    }
    return err;
}

// software reset sc7a20h.
err_t SL_Sc7a20h_Soft_Reset(void)
{
	err_t err = Sc7a20h_Write_Byte(0x23, 0x80);		//FLAG
	if (err != SWM_OK) {
        printf("write CTRL_REG4 range register failed: 0x%x\r\n", err);	//量程寄存器
        return err;
    }
	for(uint32_t iee = 0; iee < SystemCoreClock/10; iee++);		//100ms		vTaskDelay(pdMS_TO_TICKS(100))

	err = Sc7a20h_Write_Byte(0x24, 0x80);			//BOOT
	if (err != SWM_OK) {
        printf("write 0x24 register failed: 0x%x\r\n", err);
        return err;
    }
	err = Sc7a20h_Write_Byte(0x68, 0xA5);			//SOFT_RESET
	if (err != SWM_OK) {
        printf("write 0x68 register failed: 0x%x\r\n", err);
        return err;
    }
	for(uint32_t iee = 0; iee < SystemCoreClock/5; iee++);		//200ms		vTaskDelay(pdMS_TO_TICKS(200))

	uint8_t SL_Read_Reg =0xff;
	err = Sc7a20h_Read_Bytes(0x23,&SL_Read_Reg,1);	//close aoi1 function
	if (err != SWM_OK) {
        printf("read CTRL_REG4 range register failed: 0x%x\r\n", err);
        return err;
    }
	if(SL_Read_Reg==0x00)   return  SWM_OK;
	else                    return  SWM_FAIL;
}

// init sc7a20h.
err_t SL_Sc7a20h_Config(void)
{
	unsigned char Check_Flag=0x00;
	printf("check...\r\n");
	Check_Flag = SL_Sc7a20h_Check();

	if(Check_Flag==SWM_OK) {
		Check_Flag= SL_Sc7a20h_Power_Down();
		printf("power sc7a20h down, and restart.\r\n");	
	}
	printf("config sc7a20h register...\r\n");
	if(Check_Flag==SWM_OK) {
		err_t err = Sc7a20h_Write_Byte(0x1F, 0x01);//hi-pwr mode
		if (err != SWM_OK) {
			printf("write0x1F register failed: 0x%x\r\n", err);
			return err;
		}
		err = Sc7a20h_Write_Byte(CTRL_REG4, 0x98);//+-4g  high byte in lower addr DLPF open 1001100 0x88 0x98
		if (err != SWM_OK) {
			printf("write CTRL_REG4 register failed: 0x%x\r\n", err);
			return err;
		}
		err = Sc7a20h_Write_Byte(0x2E, 0x00);//BY-PASS MODE
		if (err != SWM_OK) {
			printf("write 0x2E register failed: 0x%x\r\n", err);
			return err;
		}

#if SL_SC7A20H_RAWDATA_HPF_ENABLE ==0x01	
		err = Sc7a20h_Write_Byte(0x21, 0x68);//rawdata hpf
		if (err != SWM_OK) {
			printf("write 0x21 register failed: 0x%x\r\n", err);
			return err;
		}
#else
		err = Sc7a20h_Write_Byte(CTRL_REG2, 0x00);// rawdata hpf  Enable HPF:0000 1000; Disable HPF:0000 0000
		if (err != SWM_OK) {
			printf("write 0x21 register failed: 0x%x\r\n", err);
			return err;
		}
#endif
#if		SL_SC7A20H_FIFO_ENABLE ==0x00
		err = Sc7a20h_Write_Byte(0x24, 0x80);//FIFO DISABLE
		if (err != SWM_OK) {
			printf("write 0x24 register failed: 0x%x\r\n", err);
			return err;
		}
#else
		err = Sc7a20h_Write_Byte(CTRL_REG5_A, 0xC0);//FIFO ENABLE
		if (err != SWM_OK) {
			printf("write CTRL_REG5_A register failed: 0x%x\r\n", err);
			return err;
		}
#endif
		err = Sc7a20h_Write_Byte(0x22, 0x40);//AOI1-INT1
		if (err != SWM_OK) {
			printf("write 0x22 register failed: 0x%x\r\n", err);
			return err;
		}
		err = Sc7a20h_Write_Byte(0x30, 0x2A);//XYZ ENABLE
		if (err != SWM_OK) {
			printf("write 0x30 register failed: 0x%x\r\n", err);
			return err;
		}
		err = Sc7a20h_Write_Byte(0x32, 0x0A);//>32mg*10 检测自由落体相关
		if (err != SWM_OK) {
			printf("write  自由落体的低事件  register failed: 0x%x\r\n", err);
			return err;
		}
		err = Sc7a20h_Write_Byte(0x33, 0x03);//>T*3 ��ײ���ʱ����ֵ
		if (err != SWM_OK) {
			printf("write OUT_ADC3_H register failed: 0x%x\r\n", err);
			return err;
		}
		
		//配置中断引脚的有效电平(推挽
#if SL_SC7A20H_INT_DEFAULT_LEVEL ==0x01
		err = Sc7a20h_Write_Byte(CTRL_REG6, 0x02);	//defalut high level&& push-pull
		if (err != SWM_OK) {
			printf("write CTRL_REG6 register failed: 0x%x\r\n", err);
			return err;
		}
#else
		err = Sc7a20h_Write_Byte(0x25, 0x00);//defalut low  level&& push-pull	
		if (err != SWM_OK) {
			printf("write 0x25 register failed: 0x%x\r\n", err);
			return err;
		}
#endif
	
#if SL_SC7A20H_FIFO_MODE_ENABLE ==0x00
		err = Sc7a20h_Write_Byte(0x2E, 0x9F);		//stream mode and fth=0x0F
		if (err != SWM_OK) {
			printf("write 0x2E register failed: 0x%x\r\n", err);
			return err;
		}
#endif
		for(uint32_t iee = 0; iee < SystemCoreClock/1000; iee++);	//1ms		vTaskDelay(pdMS_TO_TICKS(1))
		err = Sc7a20h_Write_Byte(CTRL_REG1, 0x57);	//工作模式,输出频率, 100Hz hi-pwr mode	0101 0111
		if (err != SWM_OK) {
			printf("write CTRL_REG1 register failed: 0x%x\r\n", err);
			return err;
		}
		for(uint32_t iee = 0; iee < SystemCoreClock/100; iee++);	//10ms		vTaskDelay(pdMS_TO_TICKS(10))

		err = Sc7a20h_Write_Byte(0x57, 0x00);		//
		if (err != SWM_OK) {
			printf("write 0x57 register failed: 0x%x\r\n", err);
			return err;
		}
		return SWM_OK;
	}
	else
		return SWM_FAIL;
}

/**
 * @brief 读取加速度数据(12位分辨率)
 * @param x X轴加速度输出(单位: mg, 范围: ±2000mg)
 * @param y Y轴加速度输出(单位: mg, 范围: ±2000mg)
 * @param z Z轴加速度输出(单位: mg, 范围: ±2000mg)
 * @return err_t ESP_OK表示成功
 *
 * 该函数一次性读取OUT_X_L_REG(0x28)到OUT_Z_H_REG(0x2D)共6个寄存器，
 * 将高低字节组合成16位数据后右移4位(12位有效数据)，
 * 根据CTRL_REG4配置的±4g量程，2mg/LSB
	| 量程 (g) | 灵敏度 (mg/LSB) |
	| ------ | ------------ |
	| ±2g    | 1 mg/LSB     |
	| ±4g    | 2 mg/LSB     |
	| ±8g    | 4 mg/LSB     |
	| ±16g   | 12 mg/LSB    |
 */
err_t sc7a20h_read_accel(Acc_Struct *sc7a20h)
{
    if (sc7a20h == NULL) {
        printf("meaningless pointer.\r\n");
        return SWM_FAIL;
    }

    uint8_t data[6] = {0};
    err_t err_XL = Sc7a20h_Read_Bytes(OUT_X_L_REG, &data[0], 1);
    if (err_XL != SWM_OK) {
        // printf("read acc data failed: 0x%x\r\n", err_XL);
        return err_XL;
    }
    err_t err_XH = Sc7a20h_Read_Bytes(OUT_X_H_REG, &data[1], 1);
    if (err_XH != SWM_OK) {
        printf("read acc data failed: 0x%x\r\n", err_XH);
        return err_XH;
    }
    err_t err_YL = Sc7a20h_Read_Bytes(OUT_Y_L_REG, &data[2], 1);
    if (err_YL != SWM_OK) {
        printf("read acc data failed: 0x%x\r\n", err_YL);
        return err_YL;
    }
    err_t err_YH = Sc7a20h_Read_Bytes(OUT_Y_H_REG, &data[3], 1);
    if (err_YH != SWM_OK) {
        printf("read acc data failed: 0x%x\r\n", err_YH);
        return err_YH;
    }
    err_t err_ZL = Sc7a20h_Read_Bytes(OUT_Z_L_REG, &data[4], 1);
    if (err_ZL != SWM_OK) {
        printf("read acc data failed: 0x%x\r\n", err_ZL);
        return err_ZL;
    }
    err_t err_ZH = Sc7a20h_Read_Bytes(OUT_Z_H_REG, &data[5], 1);
    if (err_ZH != SWM_OK) {
        printf("read acc data failed: 0x%x\r\n", err_ZH);
        return err_ZH;
    }
    
	// ±4g量程时 2mg/LSB		Acc(g)=raw×(Sensitivity mg/LSB)÷1000	(Sensitivity = 2
    sc7a20h->acc_x = (signed short)((data[1] << 8) | data[0]) >> 4;//12bit
	sc7a20h->acc_y = (signed short)((data[3] << 8) | data[2]) >> 4;//12bit
	sc7a20h->acc_z = (signed short)((data[5] << 8) | data[4]) >> 4;//12bit
	
	#if 1
	// 转换为g值(1g = 1000mg)
    sc7a20h->acc_x = sc7a20h->acc_x*0.002f;
    sc7a20h->acc_y = sc7a20h->acc_y*0.002f;
    sc7a20h->acc_z = sc7a20h->acc_z*0.002f;
	#endif
    // ESP_LOGI(TAG, "Accel X:%.2fg Y:%.2fg Z:%.2fg", sc7a20h->acc_x, sc7a20h->acc_y, sc7a20h->acc_z);
    return SWM_OK;
}
