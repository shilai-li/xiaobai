#ifndef SC7A20H_USER_H
#define SC7A20H_USER_H

#include "board.h"


typedef struct {
    float mag;
    float rms;
    float lpf_alpha;

	float acc_x,acc_y,acc_z;
    float gravity[3];           //低通滤波后的重力估计
    // float gravity_x,gravity_y,gravity_z;
    float residual[3];          //
    // float residual_x,residual_y,residual_z;

    float roll;
    float pitch;
} Acc_Struct;

typedef struct {
    // debounce counters
	uint8_t orient_counter_free_fall;	//自由落体采样
	uint8_t orient_counter_throw;		//抛起采样(抛起超重-失重-1g-失重-跌落碰撞)

	uint8_t orient_counter_faceup;		//正置采样
    uint8_t orient_counter_facedown;	//倒置采样
	uint8_t orient_counter_down2leftside;   //左侧卧采样
    uint8_t orient_counter_down2rightside;  //右侧卧采样
	uint8_t orient_counter_forward;		//前俯采样
    uint8_t orient_counter_backward;	//后仰采样

    uint8_t shake_counter_updown;		//上下晃动采样
    uint8_t shake_counter_leftright;	//左右晃动采样
    uint8_t shake_counter_forwardback;	//左右晃动采样

    // suppression timer (samples) after flip/orient change
    uint32_t flip_suppress_samples;
} DetectorState;

typedef enum {
    EVT_IDLE = 0,
	EVT_UP_THROW,
	EVT_FREE_FALL,

    EVT_ORIENT_FACEUP,
    EVT_ORIENT_FACEDOWN,
    EVT_ORIENT_LEFT,
    EVT_ORIENT_RIGHT,
    EVT_ORIENT_FORWARD,
    EVT_ORIENT_BACK,
    
	EVT_SHAKE_UPDOWN,
	EVT_SHAKE_UPSIDEDOWN_UPDOWN,
    EVT_SHAKE_LEFTRIGHT,
	EVT_SHAKE_FORWARDBACK
} event_t;

extern event_t evt;

#define SL_Sensor_Algo_Release_Enable 0x01
//0x00: FIFO-12bit;0x01: FIFO-8bit
#define SL_SC7A20H_FIFO_MODE_ENABLE   0x00 

#define SL_SC7A20H_SDO_VDD_GND            1
#define SL_SC7A20H_IIC_7BITS_8BITS        0

#if SL_SC7A20H_SDO_VDD_GND==0
#define SL_SC7A20H_IIC_7BITS_ADDR        0x18
#define SL_SC7A20H_IIC_8BITS_WRITE_ADDR  0x30
#define SL_SC7A20H_IIC_8BITS_READ_ADDR   0x31
#else
#define SL_SC7A20H_IIC_7BITS_ADDR        0x19
#define SL_SC7A20H_IIC_8BITS_WRITE_ADDR  0x32
#define SL_SC7A20H_IIC_8BITS_READ_ADDR   0x33
#endif

#if SL_SC7A20H_IIC_7BITS_8BITS==0
#define SL_SC7A20H_IIC_ADDRESS        SL_SC7A20H_IIC_7BITS_ADDR
#else
#define SL_SC7A20H_IIC_WRITE_ADDRESS  SL_SC7A20H_IIC_8BITS_WRITE_ADDR
#define SL_SC7A20H_IIC_READ_ADDRESS   SL_SC7A20H_IIC_8BITS_READ_ADDR
#endif

extern Acc_Struct SC7A20H;
extern DetectorState Counter;

//清空获取的Sc7a20h数据
void Clean_Sc7a20h_Data(Acc_Struct *sc7a20h,DetectorState *counter);

//判断并输出动作事件类型
event_t acc_task(Acc_Struct *sc7a20h,DetectorState *counter);

//依据输入打印动作类型
void Check_SC7A20H_State(event_t evt);


#ifdef __cplusplus
extern "C" {
#endif


#ifdef __cplusplus
}
#endif

#endif
