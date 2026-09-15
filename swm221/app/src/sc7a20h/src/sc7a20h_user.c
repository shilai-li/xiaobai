#include "sc7a20h_user.h"
#include "sc7a20h_driver.h"
#include "SWM221.h"
#include "board.h"
#include "Analysis.h"

#include <math.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>


#if 1
// ----------------- Define -----------------
#define M_PI		3.14f

// 防抖窗口
#define DEBOUNCE_THROW_SAMPLES 	1
#define DEBOUNCE_FALL_SAMPLES	(DEBOUNCE_THROW_SAMPLES)
#define DEBOUNCE_ORIENT_SAMPLES 5
#define DEBOUNCE_SHAKE_SAMPLES	2

// 抑制以样本数表示
#if 0
// #define ODR_HZ		100.0f
// #define DT			(1.0f / ODR_HZ)
// LPF 用于重力估计（低通）,fc_grav 推荐 0.5 Hz
// #define FC_GRAV		0.5f
// 翻转抑制 shake 时间(ms)
// #define FLIP_SUPPRESS_MS 400
// #define FLIP_SUPPRESS_SAMPLES ((uint32_t)((FLIP_SUPPRESS_MS/1000.0f) * ODR_HZ + 0.5f))
#else
#define FLIP_SUPPRESS_SAMPLES	2
#endif

// orientation thresholds on gz (g unit)
#define FREE_FALL_THR	0.3f	// mag < 0.3g 判定失重
#define THROW_THR		1.7f	// mag > 1.7g 判定抛起

#define RMS_FACEUP		0.01f
#define Z_POSITIVE_THR  1.07f	// 正置 Z > +1.07g
#define Z_NEGATIVE_THR -0.94f	// 倒置 Z < -0.94g
#define Y_POSITIVE_THR  1.0f	// 正置 Z > +1.0g
#define Y_NEGATIVE_THR -0.98f	// 倒置 Z < -0.98g
#define X_POSITIVE_THR  0.98f	// 正置 Z > +0.98g
#define X_NEGATIVE_THR -1.0f	// 倒置 Z < -1.0g

// 判定晃动阈值(g 单位)
#define RMS_SHAKE_THR	0.15f	//0.2f 0.35f
#define SHAKE_UPDOWN_THR		0.7f
#define SHAKE_UPSIDEDOWN_THR	-0.7f

// ----------------- Personal -----------------
typedef enum Weight_state{
    OVERWEIGHT = 0,
    ONE_WEIGHT,
    WEIGHTLESS
} Gravity;		//重力状态

Gravity Gra = ONE_WEIGHT;

// ------------------ Groble ------------------
event_t evt = EVT_IDLE;		//动作事件

Acc_Struct SC7A20H;			//三轴处理数据数据
DetectorState Counter;		//计数器,记录各个不同事件所处的时长,用作动作区分与判断


// ----------------- Personal -----------------
//计算重力的模长
static inline float vector_mag(Acc_Struct *sc7a20h) {
    return sqrtf(sc7a20h->acc_x*sc7a20h->acc_x + sc7a20h->acc_y*sc7a20h->acc_y + sc7a20h->acc_z*sc7a20h->acc_z);
}

//重置超/失重采样
static void Reset_Gravity_Counter(DetectorState *counter) {	
	counter->orient_counter_throw = counter->orient_counter_free_fall = 0;
}

//重置正反置采样
static void Reset_FaceUpDown_Counter(DetectorState *counter) {
	counter->orient_counter_faceup = counter->orient_counter_facedown = 0;
}

//重置左右侧卧采样
static void Reset_Down2Side_Counter(DetectorState *counter) {
	counter->orient_counter_down2leftside = counter->orient_counter_down2rightside = 0;
}

//重置前后俯仰采样
static void Reset_ForwBack_Counter(DetectorState *counter) {
	counter->orient_counter_forward = counter->orient_counter_backward = 0;
}

//重置所有朝向采样
static void Reset_Whole_Orientation_Counter(DetectorState *counter) {
	Reset_FaceUpDown_Counter(counter);
	Reset_Down2Side_Counter(counter);
	Reset_ForwBack_Counter(counter);
}

//重置摇晃采样
static void Reset_Shake_Type_Counter(DetectorState *counter) {
	counter->shake_counter_updown = 0;
	counter->shake_counter_leftright = 0;
	counter->shake_counter_forwardback = 0;
}

//超/失重采样
#define OLD_FUNCTION		0
static void Check_Gravity(Acc_Struct *sc7a20h,DetectorState *counter) {
	if((fabsf(sc7a20h->residual[2])>fabsf(sc7a20h->residual[0]))&&(fabsf(sc7a20h->residual[2])>fabsf(sc7a20h->residual[1]))) {
		if(sc7a20h->mag > THROW_THR) {
			Gra = OVERWEIGHT;						//超重
			counter->orient_counter_throw++;
			counter->orient_counter_free_fall = 0;
		} else if(sc7a20h->mag < FREE_FALL_THR) {
			#if OLD_FUNCTION
			Gra = WEIGHTLESS;						//失重
			counter->orient_counter_throw = 0;
			counter->orient_counter_free_fall++;
			#else
			Gra = WEIGHTLESS;						//失重
			// counter->orient_counter_throw = 0;
			counter->orient_counter_free_fall++;
			#endif
		} else {
			Gra = ONE_WEIGHT;						//正常重力
			Reset_Gravity_Counter(counter);
		}
	}
}

//朝向采样
static void Check_Orientation(Acc_Struct *sc7a20h,DetectorState *counter) {
	if(sc7a20h->rms<RMS_FACEUP) {
		//正置/倒置 Z
		if (sc7a20h->gravity[2] > Z_POSITIVE_THR) {
			counter->orient_counter_faceup++;
			counter->orient_counter_facedown = 0;
		} else if (sc7a20h->gravity[2] < Z_NEGATIVE_THR) {
			counter->orient_counter_faceup = 0;
			counter->orient_counter_facedown++;
		} else {
			Reset_FaceUpDown_Counter(counter);
		}
		//左侧卧/右侧卧 X
		if (sc7a20h->gravity[0] > X_POSITIVE_THR) {
			counter->orient_counter_down2leftside++;
			counter->orient_counter_down2rightside = 0;
		} else if (sc7a20h->gravity[0] < X_NEGATIVE_THR) {
			counter->orient_counter_down2leftside = 0;
			counter->orient_counter_down2rightside++;
		} else {
			Reset_Down2Side_Counter(counter);
		}
		//前俯/后仰
		if (sc7a20h->gravity[1] > Y_POSITIVE_THR) {
			counter->orient_counter_forward++;
			counter->orient_counter_backward = 0;
		} else if (sc7a20h->gravity[1] < Y_NEGATIVE_THR) {
			counter->orient_counter_forward = 0;
			counter->orient_counter_backward++;
		} else {
			Reset_ForwBack_Counter(counter);
		}
	}
}

//摇晃采样
static void Check_Shake_Type(Acc_Struct *sc7a20h,DetectorState *counter) {

}


// ------------------ Groble ------------------
//清空sc7a20h数据
void Clean_Sc7a20h_Data(Acc_Struct *sc7a20h,DetectorState *counter) {
    memset(counter, 0, sizeof(DetectorState));
    // compute LPF alpha from fc and dt
    // float tau = 1.0f / (2.0f * M_PI * FC_GRAV);
    // sc7a20h->lpf_alpha = tau / (tau + DT);
    // init gravity to 0 - optional: could warm-up with first samples externally
	memset(sc7a20h, 0, sizeof(Acc_Struct));
}

//判断并输出动作事件类型
event_t acc_task(Acc_Struct *sc7a20h,DetectorState *counter) {
	// 1. 获取原始数据
	if (sc7a20h_read_accel(sc7a20h)!=SWM_OK) {
		// printf("I2C read acc failed\r\n");
		for(uint32_t iee = 0; iee < SystemCoreClock/10; iee++);		//100ms			vTaskDelay(100 / portTICK_PERIOD_MS);

	}
	// 2. 获取低通滤波参数
	sc7a20h->lpf_alpha = 0.5;

	// 3. 低通滤波估计重力
	sc7a20h->gravity[0] = sc7a20h->lpf_alpha * sc7a20h->gravity[0] + (1.0f - sc7a20h->lpf_alpha) * sc7a20h->acc_x;
	sc7a20h->gravity[1] = sc7a20h->lpf_alpha * sc7a20h->gravity[1] + (1.0f - sc7a20h->lpf_alpha) * sc7a20h->acc_y;
	sc7a20h->gravity[2] = sc7a20h->lpf_alpha * sc7a20h->gravity[2] + (1.0f - sc7a20h->lpf_alpha) * sc7a20h->acc_z;
	
	// 4. 动态分量 residual = 加速度 - 重力
	sc7a20h->residual[0] = sc7a20h->acc_x - sc7a20h->gravity[0];
	sc7a20h->residual[1] = sc7a20h->acc_y - sc7a20h->gravity[1];
	sc7a20h->residual[2] = sc7a20h->acc_z - sc7a20h->gravity[2];
	
	// 取正,比较大小
	float abs_dx = fabs(sc7a20h->residual[0]);
	float abs_dy = fabs(sc7a20h->residual[1]);
	float abs_dz = fabs(sc7a20h->residual[2]);

	// printf("g:%0.2f,%0.2f,%0.2f	rms:%0.2f,acc:%0.2f,%0.2f,%0.2f\r\n",sc7a20h->gravity[0],sc7a20h->gravity[1],sc7a20h->gravity[2],sc7a20h->rms,sc7a20h->residual[0],sc7a20h->residual[1],sc7a20h->residual[2]);

	// 5. RMS 残差	(加速度模长(去除重力)
	sc7a20h->rms = sqrtf((sc7a20h->residual[0]*sc7a20h->residual[0] + 
					sc7a20h->residual[1]*sc7a20h->residual[1] + 
					sc7a20h->residual[2]*sc7a20h->residual[2]) / 3.0f);

	// 6. 模长
	sc7a20h->mag = vector_mag(sc7a20h);
	
	// 7. 计算 Roll / Pitch(简易版)
	// float roll  = atan2f(sc7a20h->gravity[1], sc7a20h->gravity[2]) * 180.0f / M_PI;
	// float pitch = atan2f(-sc7a20h->gravity[0], sqrtf(sc7a20h->gravity[1]*sc7a20h->gravity[1] + sc7a20h->gravity[2]*sc7a20h->gravity[2])) * 180.0f / M_PI;
	
	// 8. 打印结果
	// printf("ACC[g]: (%.2f, %.2f, %.2f), |mag|=%.2f, RMS=%.2f, Roll=%.1f, Pitch=%.1f\r\n",
	//          sc7a20h->acc_x,sc7a20h->acc_y,sc7a20h->acc_z, mag, rms, roll, pitch);
	
	// 9. 动作检测
	// 检测 抛起&自由落体:
	#if 1
	// 抛起(超重):mag模长大于1,重力大于1; 自由落体(失重):mag模长小于1,重力小于1
	Check_Gravity(sc7a20h,counter);
	#if OLD_FUNCTION
	if (counter->orient_counter_throw >= DEBOUNCE_THROW_SAMPLES) {
        evt = EVT_UP_THROW;
        counter->flip_suppress_samples = FLIP_SUPPRESS_SAMPLES;
        Reset_Gravity_Counter(counter);
        return evt;
    } else if (counter->orient_counter_free_fall >= DEBOUNCE_FALL_SAMPLES) {
        evt = EVT_FREE_FALL;
        counter->flip_suppress_samples = FLIP_SUPPRESS_SAMPLES;
        Reset_Gravity_Counter(counter);
        return evt;
    }
	#else //加入超失重力检测
    if (counter->orient_counter_free_fall >= DEBOUNCE_FALL_SAMPLES) {
		if (counter->orient_counter_throw >= DEBOUNCE_THROW_SAMPLES) {
			evt = EVT_UP_THROW;
			counter->flip_suppress_samples = FLIP_SUPPRESS_SAMPLES;
			Reset_Gravity_Counter(counter);
			return evt;
		} else {
			evt = EVT_FREE_FALL;
			counter->flip_suppress_samples = FLIP_SUPPRESS_SAMPLES;
			Reset_Gravity_Counter(counter);
			return evt;
		}
    }
	#endif
	#endif

	// 检测 静置状态:
	#if 1
	// 静置		正置,mag模长为1,重力为1g; 倒置,mag模长为1,重力为-1g
	Check_Orientation(sc7a20h,counter);
	// printf("gravity x:%.2fg y:%.2fg z:%.2fg\r\n",
    //                 sc7a20h->gravity[0], sc7a20h->gravity[1], sc7a20h->gravity[2]);
	if (counter->orient_counter_faceup >= DEBOUNCE_ORIENT_SAMPLES) {
        evt = EVT_ORIENT_FACEUP;
        counter->flip_suppress_samples = FLIP_SUPPRESS_SAMPLES;	//添加晃动抑制期
        Reset_Whole_Orientation_Counter(counter);
        return evt;
    } else if (counter->orient_counter_facedown >= DEBOUNCE_ORIENT_SAMPLES) {
        evt = EVT_ORIENT_FACEDOWN;
        counter->flip_suppress_samples = FLIP_SUPPRESS_SAMPLES;
        Reset_Whole_Orientation_Counter(counter);
        return evt;
    } else if(counter->orient_counter_down2leftside >= DEBOUNCE_ORIENT_SAMPLES) {
		evt = EVT_ORIENT_LEFT;
        counter->flip_suppress_samples = FLIP_SUPPRESS_SAMPLES;
		Reset_Whole_Orientation_Counter(counter);
		return evt;
	} else if(counter->orient_counter_down2rightside >= DEBOUNCE_ORIENT_SAMPLES) {
		evt = EVT_ORIENT_RIGHT;
        counter->flip_suppress_samples = FLIP_SUPPRESS_SAMPLES;
		Reset_Whole_Orientation_Counter(counter);
		return evt;
	} else if(counter->orient_counter_forward >= DEBOUNCE_ORIENT_SAMPLES) {
		evt = EVT_ORIENT_FORWARD;
        counter->flip_suppress_samples = FLIP_SUPPRESS_SAMPLES;
		Reset_Whole_Orientation_Counter(counter);
		return evt;
	} else if(counter->orient_counter_backward>=DEBOUNCE_ORIENT_SAMPLES) {
		evt = EVT_ORIENT_BACK;
        counter->flip_suppress_samples = FLIP_SUPPRESS_SAMPLES;
		Reset_Whole_Orientation_Counter(counter);
		return evt;
	}
	#endif


    // 检测 Shake Type:
	#if 1
	// 晃动,mag模长小于1
	if (counter->flip_suppress_samples > 0) {
        // 在抑制期中，减少计时器并不做 shake 报告
        counter->flip_suppress_samples--;
    } else {
		// printf("rms:%0.2f\r\n\n",sc7a20h->rms);
		// 判定是否有显著动态:
		if (sc7a20h->rms >= RMS_SHAKE_THR) {
			// printf("gravity x:%.2fg y:%.2fg z:%.2fg\r\n",
			// 				abs_dx, abs_dy, abs_dz);
			//判断主轴:
			if (abs_dz>abs_dx&&abs_dz>abs_dy) {
				// 判为 Z 主导(上下)
				counter->shake_counter_updown++;
				counter->shake_counter_leftright = 0;
				counter->shake_counter_forwardback = 0;
			} else if(abs_dx>abs_dz&&abs_dx>abs_dy) {
				// 判为 X 主导(左右)
				counter->shake_counter_updown = 0;
				counter->shake_counter_leftright++;
				counter->shake_counter_forwardback = 0;
			} else if(abs_dy>abs_dz&&abs_dy>abs_dx) {
				// 判为 Y 主导(前后)
				counter->shake_counter_updown = 0;
				counter->shake_counter_leftright = 0;
				counter->shake_counter_forwardback++;
			}

			// 依据主轴判断晃动动作
			if (counter->shake_counter_updown >= DEBOUNCE_SHAKE_SAMPLES) {
				if(sc7a20h->acc_z>SHAKE_UPDOWN_THR){
					evt = EVT_SHAKE_UPDOWN;
				} else if(sc7a20h->acc_z<SHAKE_UPSIDEDOWN_THR) {
					evt = EVT_SHAKE_UPSIDEDOWN_UPDOWN;
				}
				Reset_Shake_Type_Counter(counter);
				return evt;
			} else if(counter->shake_counter_leftright >= DEBOUNCE_SHAKE_SAMPLES) {
				evt = EVT_SHAKE_LEFTRIGHT;
				Reset_Shake_Type_Counter(counter);
				return evt;
			} else if(counter->shake_counter_forwardback >= DEBOUNCE_SHAKE_SAMPLES) {
				evt = EVT_SHAKE_FORWARDBACK;
				Reset_Shake_Type_Counter(counter);
				return evt;
			}
		} else {
			// printf("here\r\n");
			// 动态不足，重置 shake counters
			Reset_Shake_Type_Counter(counter);
			return EVT_IDLE;
		}
    }
	#endif
    
	return EVT_IDLE;
}

//依据输入打印动作类型
void Check_SC7A20H_State(event_t evt) {
	
	switch (evt) {
		case EVT_IDLE:
			// printf("NONE\n");				//无动作
			break;
		
		case EVT_UP_THROW:
			printf("EVT_UP_THROW\n");			//抛起
			break;
		case EVT_FREE_FALL:
			printf("EVT_FREE_FALL\n");			//自由落体
			break;

		case EVT_ORIENT_FACEUP:
			printf("EVT_ORIENT_FACEUP Z\n");	//正置
			break;
		case EVT_ORIENT_FACEDOWN:
			printf("EVT_ORIENT_FACEDOWN Z\n");	//倒置
			break;
		case EVT_ORIENT_LEFT:
			printf("EVT_ORIENT_LEFT X\n");		//左侧卧
			break;
		case EVT_ORIENT_RIGHT:
			printf("EVT_ORIENT_RIGHT X\n");		//右侧卧
			break;
		case EVT_ORIENT_FORWARD:
			printf("EVT_ORIENT_FORWARD Y\n");	//前俯
			break;
		case EVT_ORIENT_BACK:
			printf("EVT_ORIENT_BACK Y\n");		//后仰
			break;

		case EVT_SHAKE_UPDOWN:
			printf("EVT_SHAKE_UPDOWN Z\n");		//上下晃动
			break;
		case EVT_SHAKE_UPSIDEDOWN_UPDOWN:
			printf("EVT_SHAKE_UPSIDEDOWN_UPDOWN Z\n");	//倒置上下晃动
			break;
		case EVT_SHAKE_LEFTRIGHT:
			printf("EVT_SHAKE_LEFTRIGHT X\n");	//左右晃动
			break;
		case EVT_SHAKE_FORWARDBACK:
			printf("EVT_SHAKE_FORWARDBACK Y\n");//前后晃动
			device.evt.sta_now = 0x01;
			break;
//		
	}
	for(uint32_t iee = 0; iee < SystemCoreClock/20; iee++);		//50ms		vTaskDelay(50 / portTICK_PERIOD_MS);

}

#endif

