#include "stdlib.h"
#include "board.h"
#include "chry_ringbuffer.h"
#include "synwit_ui_framework/synwit_serial_display.h"
#include "serial_display_process.h"

#define UART_DISP		    UART1			//dev_uart.c的初始化串口
#define UART_BUFF_MAX_LEN	256			    //串口数据接收处理buff
#define CRC_NEED			1			    //校验位启动宏定义，置1开启校验功能,默认与client一致（置1）

/* 环形缓冲区缓存 */
static unsigned char rb_Buff[UART_BUFF_MAX_LEN] = {0};

/* 串口接收数据缓存 */
static unsigned char revBuff[UART_BUFF_MAX_LEN] = {0};
static unsigned char respBuff[UART_BUFF_MAX_LEN] = {0};

static chry_ringbuffer_t rb;

void uart_readbyte_hook_for_sdisp(uint8_t chr)
{
    if (!rb.pool) 
        return;
    
    chry_ringbuffer_write_byte(&rb, chr);
}

/**
 * @brief   计算校验值
 * 
 * @param[in]   dataBuff        数据内容
 * @param[in]   dataLen         数据长度
 * @return  unsigned int        返回校验和
 */
static uint16_t Uart_CRC_CCITT(const uint8_t* msg, uint32_t len)
{
    uint16_t crc = 0x0000;
    uint16_t polynomial = 0x1021;

    for (uint32_t i = 0; i < len; i++) {
        crc ^= (msg[i] << 8);
        for (unsigned char j = 0; j < 8; j++) {
            if (crc & 0x8000) {
                crc = (crc << 1) ^ polynomial;
            }
            else {
                crc <<= 1;
            }
        }
    }
    return crc & 0xFFFF;
}


/**
 * @brief 环形缓冲区初始化
 * 
 */
static void sdisp_ringbuff_init_once(void)
{
    static bool sdisp_uart_inited = false;
    
    if(sdisp_uart_inited == false) {
		
		if (0 == chry_ringbuffer_init(&rb, rb_Buff, 256)) {
			//printf("chry_ringbuffer_init success\r\n");
		} else {
			printf("chry_ringbuffer_init error\r\n");
			while (1) __NOP();
		}
        
        sdisp_uart_inited = true;
	}
}


/**
 * @brief   msg封包发送
 * 
 * @param[in]   msg    自定义信息  
 * @param[in]	len	   信息数据长度
 */
void sdisp_notify(const uint8_t *data, uint32_t len)
{
	uint32_t frame_len = len + 2 + 2 + 2;    // 2bytes len + 2 bytes header + 2 bytes crc
	uint8_t *buf = (uint8_t *)malloc(frame_len); 
	if (buf == NULL) {
		// 处理内存分配错误
		printf("Error allocating memory for data\r\n");
		return;
	}
	
	buf[0] =  0xFB;
	buf[1] =  0x83;
	
	buf[2] = (uint8_t)(len & 0xFF);
    buf[3] = (uint8_t)(len >> 8);
	
	memcpy(&buf[4], data, len);

	//进行帧校验计算
	unsigned int crc = Uart_CRC_CCITT(buf, len + 4);
	buf[len + 4]   =  (uint8_t)(crc & 0xFF);
	buf[len + 4 + 1] =  (uint8_t)(crc >> 8);

	sdisp_write(buf, frame_len);
	free(buf);
}

/**
 * @brief   错误码反馈
 * 
 * @param[in]   err_code       错误码
 */
static void response_rx(uint8_t err_code)
{
    uint8_t tx_buf[] = {0xFB, 0xFF, 0x00, 0x00, 0x00};
    tx_buf[2] = err_code;

    uint16_t crc = Uart_CRC_CCITT(tx_buf, 3) & 0xFFFF;
    tx_buf[3] = crc & 0xFF;
    tx_buf[4] = crc >> 8;

    sdisp_write(tx_buf, sizeof(tx_buf));
}

/**
 * @brief   校验值比对
 * 
 * @param[in]   crc_value       校验计算值
 * @return  	true    校验通过	false	检验失败
 */
static bool UART_Check_CRC(unsigned int crc_value)
{

    uint8_t CRC_buff[2];
    chry_ringbuffer_read(&rb, CRC_buff, 2);
    if (crc_value == ((CRC_buff[1] << 8) + CRC_buff[0]))
    {
        //校验正确。
        //printf("crc check ok\r\n");
        return true;
    }
    else
    {
        //校验错误,返回出错帧，默认0码
        response_rx(0);
        //printf("crc check err\r\n");
        return false;
    }

}

/**
 * @brief uart发送函数 
 * uint8_t *buf 发送数据的地址 
 * uint8_t len 发送数据的长度
 */
extern void uart_send_msg(UART_TypeDef *uart_x, const uint8_t *data, uint32_t bytes);
void sdisp_write(uint8_t *buf, int len)
{
	uart_send_msg(UART_DISP, buf, len);
}

/* 定义包头及指令信息 */
#define HEADER0 0xFA  		// 起始码
#define HEADER_setter 0x80  // 起始码_setter

/* 枚举读取数据报文的状态 */
typedef enum
{
    FSM_IDLE,
	FSM_HEADER,
	FSM_LEN,
	FSM_END
} rx_fsm_t;

/* 定义数据包接收状态的变量,并初始化为空闲状态 */
typedef struct rx
{
    rx_fsm_t fsm;
    uint32_t len;          // 指令参数长度
} rx_t;

static rx_t rx = {FSM_IDLE, 0};

/**
 * @brief uart接收函数，需要在main_tick或其他函数循环调用
 * 
 */
void sdisp_handler()
{
    uint8_t receivedbyte;
    static uint32_t timeout_start = 0;
	static int pos = 0;
    static uint32_t resp_len = 0;
	
    sdisp_ringbuff_init_once();
    if (chry_ringbuffer_get_used(&rb) < UART_BUFF_MAX_LEN - 1) // 缓存中有数据
    {   
        switch (rx.fsm)
        {
			case FSM_IDLE:
				chry_ringbuffer_read(&rb, &receivedbyte, 1);
				if(HEADER0 == receivedbyte)
				{
					
					revBuff[0] = receivedbyte;
					if (chry_ringbuffer_get_used(&rb))
					{
						chry_ringbuffer_read(&rb, &receivedbyte, 1);
						
						if (HEADER_setter == receivedbyte)
						{
                            timeout_start = systick_get_tick();
							revBuff[1] = receivedbyte;
							rx.fsm = FSM_HEADER;
						}
						else
						{
							//帧头2错误处理
							//to do...
							//printf ("header1 err\r\n");
						}
					}
				}
				else
				{
					//帧头1错误处理
					//to do...
					//printf ("header1 err\r\n");
				}
			break;
			
			case FSM_HEADER:
				if (chry_ringbuffer_get_used(&rb) >= 2)
				{
					chry_ringbuffer_read(&rb, &revBuff[2], 2); // 指令长度
					rx.len = (revBuff[3] << 8) + (revBuff[2] << 0);	
					timeout_start = systick_get_tick();
					rx.fsm = FSM_LEN;
				}
			break;
				
			case FSM_LEN:
                #if (CRC_NEED)
				if (chry_ringbuffer_get_used(&rb) >= rx.len + 2)
				{
					chry_ringbuffer_read(&rb, &revBuff[4], rx.len); // 读取数据层内容
					
					uint16_t crc = Uart_CRC_CCITT(revBuff, rx.len+4) ; //进行crc校验
					if(UART_Check_CRC(crc))
					{
						//校验通过，除去校验位，数据丢入处理
						resp_len = synwit_ug_sdcmd_run((const uint8_t*)revBuff,respBuff,sizeof(respBuff),NULL);
                        
                        uint16_t resp_crc = Uart_CRC_CCITT(respBuff, resp_len);
                        respBuff[resp_len] = resp_crc & 0xFF;
                        respBuff[resp_len+1] = resp_crc >> 8;
                        sdisp_write(respBuff, resp_len+2);
					}
					else
					{
						//校验错误，返回错误提示
					}

					timeout_start = systick_get_tick();
					rx.fsm = FSM_END;
				}
                #else
                if (chry_ringbuffer_get_used(&rb) >= rx.len)
				{
					chry_ringbuffer_read(&rb, &revBuff[4], rx.len); // 读取数据层内容
                    //数据丢入处理
					resp_len = synwit_ug_sdcmd_run((const uint8_t*)revBuff,respBuff,sizeof(respBuff),NULL);
					sdisp_write(respBuff, resp_len);

					timeout_start = systick_get_tick();
					rx.fsm = FSM_END;
				}
                #endif
			break;
				
			case FSM_END:
				rx.len = 0;
				resp_len = 0;
				memset(respBuff, 0, sizeof(respBuff));
				memset(revBuff, 0, sizeof(revBuff));
				rx.fsm = FSM_IDLE;
			break;
        }
    }
	else
	{
		chry_ringbuffer_reset(&rb);
		rx.len = 0;
		resp_len = 0;		
		memset(respBuff, 0, sizeof(respBuff));
		memset(revBuff, 0, sizeof(revBuff));	
		rx.fsm = FSM_IDLE;
	}
	
	if((rx.fsm != FSM_IDLE) &&
        (systick_get_tick() - timeout_start)>=2000)
	{
		timeout_start = systick_get_tick();
		rx.len = 0;
		resp_len = 0;
		memset(respBuff, 0, sizeof(respBuff));
		memset(revBuff, 0, sizeof(revBuff));	
		rx.fsm = FSM_IDLE;
	}
}
