#include <string.h>
#include <ctype.h>
#include "ugui/ugui.h"
#include "synwit_ui_framework/synwit_ui.h"
#include "synwit_ui_framework/synwit_ui_internal.h"
#include "ui_src/appkit/screen_id.h"

#include "board.h"
#include "app_cfg.h"
#include "board\dev_servo.h"
#include "ui_src/appkit/app.h"
#include "CircleBuffer.h"
#include "SWM221_sleep.h"
#include "Analysis.h"
#include "sc7a20h_user.h"
#include "sc7a20h_driver.h"

extern void face_timeout_task(void);
extern void send_audio_play_cmd(uint16_t voice_id);
extern uint32_t systick_get_tick(void);

#if (ENABLED_SERIAL_DISPLAY == 1)
#include "serial_display/serial_display_process.h"
#endif

/* 使能 TEST_CASE_EN 宏将执行单元测试用例: LED / KEY / TP / UART-Modbus / screen_demo, 
 * 反之若屏蔽该宏, 则仅保留 LCD 驱动显示.
 */
//#define TEST_CASE_EN 
#ifdef TEST_CASE_EN
#define __IMPORT_TEST_CASE(__case_name)     extern void test_case##_##__case_name(void); test_case##_##__case_name()
#else 
#define __IMPORT_TEST_CASE(__case_name)     
#endif

#define DISP_WIDTH     CFG_LCD_HDOT
#define DISP_HEIGHT    CFG_LCD_VDOT

/* 分配给显示缓存的像素个数(必须大于或等于屏幕宽度) */
#define DISP_PFB_PIXELS    (DISP_WIDTH * 1)
__USED static MEM_UNIT platform_heap[HEAP_SIZE / sizeof(MEM_UNIT)];

/* driver */
static void driver_init(void);
/*static*/ lcd_mpu_desc_t LCD_Obj;
#define This_LCD        (&LCD_Obj)
static volatile uint8_t ISR_Flag_LCD_DMA = 0; /* 0: idle  1: busy */
static bool startup_backlight_pending = false;
static uint32_t backlight_first_frame_tick = 0;
#define BACKLIGHT_FRAME_SETTLE_MS 300U

/*uart*/
volatile bool msg_rcvd = false;			//中断中,volatile防止Ofast优化 (逻辑出错
uint8_t buf_count = 0;
uint8_t BufRec[32];
CircleBuffer_t CirBuf;

/*usart1*/
struct android_cbuf android_receive_buf;  //安卓接收缓冲区
char android_Buf_send[64] = {0};		  		//发送缓冲区() 


/*以下两个变量需要在screen_id.h中引用*/
volatile int finish_play_flag = 1;//序列播放结束标志
volatile int sleep_interupt_play_flag = 0;//序列播放结束标志,当动画还没有播放完成就收到了休眠的消息，需要把当前动画播完，避免唤醒后默认接着播放
/**/
volatile int finish_a_group_flag = 0;//一个动画播放完成
volatile uint8_t test_driver_change = 0; //切换同向（1）、异向（0），默认保留100/301原方向
uint8_t last_driver = 0; //记录上一次的同、异状态
volatile int lcd_init_time = 1; //lcd第一次初始化
int first_power_on = 1;
int play_wait_action = 1 ;//没有收到串口指令后，保证有画面

//protocol:
uint8_t Anima_FinishBuf[4] = {0x01, 0x03, 0x0A, 0x89};		//the anima play-over protocol
uint8_t testbuf[3] = {0x00,0x88,0x88};

void gpio_outhigh(void);


void GPIOB1_GPIOA9_DMA_Handler(void)
{
    if (0 == lcd_mpu_flush_bitmap_done(This_LCD)) {
        ISR_Flag_LCD_DMA = 0; // report platform(DMA support SPI 8bit / i8080 8bit / QSPI)
    }
}

static uint8_t QSPI_Multiplex_Flag = 0;//0: Init    1: LCD    2: Flash

void qspi_multiplex_lcd(void)
{
#if CFG_LCD_IF == CFG_LCD_IF_QSPI
    if (1 != QSPI_Multiplex_Flag) {
        QSPI_Multiplex_Flag = 1;
        flash_qspi_port_deinit();
        lcd_mpu_port_init(This_LCD);
		printf("qspi_multiplex_lcd\r\n");
    }
#endif
}

void qspi_multiplex_flash(void)
{
#if CFG_LCD_IF == CFG_LCD_IF_QSPI
    if (2 != QSPI_Multiplex_Flag) {
        QSPI_Multiplex_Flag = 2;
        lcd_mpu_port_deinit(This_LCD);
        flash_qspi_port_init();
		printf("qspi_multiplex_flash\r\n");
    }
#endif
}

static void flush_start(UG_S16 x, UG_S16 y, UG_S16 w, UG_S16 h)
{
     /* 页面某个区域开始刷新前会触发这个回调，
        TE(防撕裂)信号的同步处理可以加在里
      */
}

static void area_set(UG_S16 xs, UG_S16 ys, UG_S16 w, UG_S16 h)
{
//    printf("%s: xs, ys, w, h[%d, %d, %d, %d]\r\n", __FUNCTION__, xs, ys, w, h);
    lcd_mpu_flush_bitmap_wait(This_LCD, &ISR_Flag_LCD_DMA); // async wait

    qspi_multiplex_lcd(); // QSPI 分时复用 LCD 
    
    lcd_mpu_set_disp_area(This_LCD, xs, xs + w - 1, ys, ys + h - 1);
//	printf("area_set\r\n");
}

static void flush_pixels(UG_COLOR *colors, UG_U16 num, UG_U8 asyncable)
{
//    printf("%s: colors[0x%p], num[%d], asyncable[%d]\r\n", __FUNCTION__, colors, num, asyncable);
    lcd_mpu_flush_bitmap_wait(This_LCD, &ISR_Flag_LCD_DMA); // async wait

    qspi_multiplex_lcd(); // QSPI 分时复用 LCD 
//    	printf("flush_pixels\r\n");
    if (asyncable) {
        ISR_Flag_LCD_DMA = 1; //[async] set busy before [flush_bitmap] start
        if (0 == lcd_mpu_flush_bitmap_async(This_LCD, colors, num)) {
            return;
        }
        ISR_Flag_LCD_DMA = 0; // Fallback
    }
    lcd_mpu_flush_bitmap(This_LCD, colors, num); // sync
}

static void flush_ready(UG_S16 x, UG_S16 y, UG_S16 w, UG_S16 h)
{
    (void)x;
    (void)w;

    if (startup_backlight_pending &&
        backlight_first_frame_tick == 0U &&
        (y + h) >= DISP_HEIGHT) {
        /* flush_ready may run once per rendered strip. Wait for the strip that
         * reaches the last display row, then start a settling interval. */
        lcd_mpu_flush_bitmap_wait(This_LCD, &ISR_Flag_LCD_DMA);
        backlight_first_frame_tick = systick_get_tick();
    }
}

void display_hide_until_full_flush(void)
{
    GPIO_ClrBit(GPIOB, PIN6);
    backlight_first_frame_tick = 0;
    startup_backlight_pending = true;
}

static void display_backlight_startup_task(void)
{
    if (startup_backlight_pending &&
        backlight_first_frame_tick != 0U &&
        (uint32_t)(systick_get_tick() - backlight_first_frame_tick) >= BACKLIGHT_FRAME_SETTLE_MS) {
        lcd_mpu_flush_bitmap_wait(This_LCD, &ISR_Flag_LCD_DMA);
        GPIO_SetBit(GPIOB, PIN6);
        startup_backlight_pending = false;
    }
}

static int ui_data_read(UG_U32 offset, UG_U32 size, void *buf)
{
   	// printf("%s: offset[0x%x], size[%d]\r\n", __FUNCTION__, offset, size);
    qspi_multiplex_flash(); // QSPI 分时复用 Flash 
    
    qspi_dma_read(offset, buf, size, 2, 2);
	
   	// printf("ui_data_read\r\n");
    return 0;
}

static void framework_ready(void)
{
    _ug_app_screen_register();
}

void sleep_out_open_backlight(void)
{
	GPIO_SetBit(GPIOB, PIN6);
}

static void app_ready(void)
{
    /* Keep the backlight off until flush_ready confirms that the first frame
     * has completely reached both LCD controllers. */
    GPIO_Init(GPIOB, PIN6, 1, 0, 0, 0);
    display_hide_until_full_flush();
    synwit_ug_load_screen(SCREEN100);
}

static void pre_blue(const char* msg)
{
    // 蓝屏前确保打开背光
  lcd_mpu_set_backlight(This_LCD, 0);
	GPIO_Init(GPIOB, PIN6, 1, 0, 0, 0);			//Êä³ö£¬ ½ÓLED
	GPIO_SetBit(GPIOB, PIN6);
}

/*KEY*/
#define POWER_ON GPIO_SetBit(GPIOA, PIN6)
#define POWER_OFF GPIO_ClrBit(GPIOA, PIN6)
#define KEY_SCAN_INTERVAL_MS       10
#define POWER_OFF_LONG_PRESS_MS  3000
/* The CI1303 reports Idle when prompt playback finishes, so the shutdown wait
 * ends on that report instead of on a fixed delay. Preempting an active
 * playback makes it report Idle for the interrupted clip first, within a few
 * milliseconds of the play command, so reports arriving inside
 * POWER_OFF_AUDIO_MIN_MS are ignored. The shortest shutdown prompt runs about
 * 2.7 s and the longest about 4.5 s, leaving that window unambiguous.
 * POWER_OFF_AUDIO_TIMEOUT_MS only covers a CI1303 that never reports. */
#define CI1303_PLAY_DONE_STATE      2213U
#define CI1303_LISTENING_STATE      2212U
#define CI1303_SPEAKING_STATE       2214U
#define CI1303_USER_SPEAKING_STATE  2215U
#define CI1303_TOPIC_SPEAKING_STATE 2216U
/* Fallback bound on topic playback, used until the playback-end report arrives
 * and replaces it with the real reply window. Far longer than any announcement:
 * it only has to keep a lost or never-sent Idle report from leaving the panel
 * lit for good, because nothing else re-arms the deadline. */
#define HIBERNATION_TOPIC_PLAYBACK_MAX_MS 120000UL
#define POWER_OFF_AUDIO_MIN_MS      1000
#define POWER_OFF_AUDIO_TIMEOUT_MS  8000
/* Used until the backend sends its sleepTime. The panel must not stay lit
 * forever if that packet never arrives. */
#define HIBERNATION_TIMEOUT_DEFAULT_SECONDS 60U
#define HIBERNATION_VOICE_FIRST_ID  5010U
#define HIBERNATION_VOICE_COUNT     5U
#define POWER_OFF_VOICE_FIRST_ID    5015U
#define POWER_OFF_VOICE_COUNT       5U
int power_on_flag = 0;
static volatile bool power_off_requested = false;
static volatile bool power_off_pending = false;
static volatile bool power_off_audio_done = false;
static uint32_t power_off_audio_start_tick = 0;
static volatile bool local_activity_pending = false;
static bool hibernation_active = false;
static bool hibernation_topic_wake = false;
static uint32_t hibernation_last_activity_tick = 0;
static uint32_t hibernation_topic_wake_deadline = 0;
static uint32_t hibernation_exit_wakeup_window_ms = 30000UL;
static uint32_t hibernation_timeout_ms =
    HIBERNATION_TIMEOUT_DEFAULT_SECONDS * 1000UL;
static volatile bool ci1303_voice_active = false;
static uint16_t hibernation_voice_queue[HIBERNATION_VOICE_COUNT] = {
    5010U, 5011U, 5012U, 5013U, 5014U
};
static uint8_t hibernation_voice_queue_index = HIBERNATION_VOICE_COUNT;
static uint32_t hibernation_voice_prng_state = 0;

static uint32_t hibernation_voice_prng_next(void)
{
    uint32_t value = hibernation_voice_prng_state;

    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;
    hibernation_voice_prng_state = value;
    return value;
}

static uint16_t hibernation_next_voice_id(void)
{
    uint8_t i;

    if (hibernation_voice_queue_index >= HIBERNATION_VOICE_COUNT) {
        if (hibernation_voice_prng_state == 0) {
            hibernation_voice_prng_state = systick_get_tick() ^ 0x6D2B79F5UL;
        }

        for (i = 0; i < HIBERNATION_VOICE_COUNT; ++i) {
            hibernation_voice_queue[i] = HIBERNATION_VOICE_FIRST_ID + i;
        }

        for (i = HIBERNATION_VOICE_COUNT - 1; i > 0; --i) {
            uint8_t j = (uint8_t)(hibernation_voice_prng_next() % (i + 1));
            uint16_t voice_id = hibernation_voice_queue[i];
            hibernation_voice_queue[i] = hibernation_voice_queue[j];
            hibernation_voice_queue[j] = voice_id;
        }
        hibernation_voice_queue_index = 0;
    }

    return hibernation_voice_queue[hibernation_voice_queue_index++];
}

static void hibernation_enter(void)
{
    uint16_t voice_id = hibernation_next_voice_id();

    /* Hibernation keeps the ESP32-C3 + ML307C rail enabled. Only the LCD
     * panel/backlight is put into low power. */
    lcd_mpu_set_power(This_LCD, 0);
    GPIO_SetBit(GPIOA, PIN13);
    send_audio_play_cmd(voice_id);
    hibernation_active = true;
    hibernation_topic_wake = false;
    hibernation_topic_wake_deadline = 0;
    printf(">>> hibernation_enter: LCD off, ESP32/4G enabled, CI1303 voice %u <<<\r\n",
           voice_id);
}

/* Return to black after the unanswered-topic listening window. The normal idle
 * countdown was never refreshed, so the goodnight prompt would be wrong a
 * second time. */
static void hibernation_reenter_quiet(void)
{
    lcd_mpu_set_power(This_LCD, 0);
    hibernation_active = true;
    hibernation_topic_wake = false;
    hibernation_topic_wake_deadline = 0;
    printf(">>> hibernation: unanswered topic window ended, LCD off again <<<\r\n");
}

static void hibernation_exit(void)
{
    GPIO_SetBit(GPIOA, PIN13);
    lcd_mpu_set_power(This_LCD, 1);
    hibernation_active = false;
    printf(">>> hibernation_exit: wake notification/input <<<\r\n");
}

void hibernation_notify_ci1303_state(uint16_t state)
{
    if (power_off_pending) {
        /* Only the shutdown prompt matters now. Skipping the hibernation
         * bookkeeping also keeps a Speaking report from re-powering the
         * panel while the board is shutting down. */
        if (state == CI1303_PLAY_DONE_STATE &&
            (uint32_t)(systick_get_tick() - power_off_audio_start_tick) >=
                POWER_OFF_AUDIO_MIN_MS) {
            power_off_audio_done = true;
        }
        return;
    }

    /* Still means "a cloud conversation is on air", which is what the touch
     * gesture guard needs. It is no longer what feeds the idle countdown. */
    ci1303_voice_active = (state == CI1303_LISTENING_STATE ||
                           state == CI1303_SPEAKING_STATE ||
                           state == CI1303_TOPIC_SPEAKING_STATE);

    switch (state) {
    case CI1303_USER_SPEAKING_STATE:
        /* The one report that means the user is talking to the device, and so
         * the only one that refreshes the idle countdown. It also promotes a
         * panel a topic had merely borrowed into a normally lit one. */
        hibernation_topic_wake = false;
        hibernation_last_activity_tick = systick_get_tick();
        if (hibernation_active) {
            hibernation_exit();
        }
        break;

    case CI1303_SPEAKING_STATE:
        /* A normal cloud reply may still race the panel's idle transition. */
        if (hibernation_active) {
            hibernation_exit();
        }
        break;

    case CI1303_TOPIC_SPEAKING_STATE:
        /* ESP32 marks topic playback explicitly, so this works whether the
         * panel was already off, still lit, or crossed its idle deadline while
         * the topic was playing. PLAY_DONE replaces this fallback with the real
         * reply window; arming one here is what bounds a report that never
         * arrives, since the branch below owns the panel until it expires. */
        hibernation_topic_wake = true;
        hibernation_topic_wake_deadline = systick_get_tick() +
            HIBERNATION_TOPIC_PLAYBACK_MAX_MS + hibernation_exit_wakeup_window_ms;
        if (hibernation_active) {
            hibernation_exit();
        }
        break;

    case CI1303_LISTENING_STATE:
        /* Only enter_wakeup_deal() reports this, and that is the wake word and
         * key path. The window that follows a topic goes through
         * enter_wakeup_deal_v1(), which reports no state at all, so this is
         * always the user reaching for the device. Act on it even when a topic
         * already lit the panel: the wake word has to take the screen over from
         * the topic, or the topic's Idle would black it out mid-conversation. */
        hibernation_topic_wake = false;
        hibernation_last_activity_tick = systick_get_tick();
        if (hibernation_active) {
            hibernation_exit();
        }
        break;

    case CI1303_PLAY_DONE_STATE:
        /* CI1303 reports Idle when topic audio playback ends, before the
         * backend opens its configured wakeup window. Keep a panel borrowed by
         * the topic lit until that full window has elapsed. User speech or a
         * wake word clears hibernation_topic_wake in the cases above. */
        if (hibernation_topic_wake) {
            hibernation_topic_wake_deadline = systick_get_tick() +
                hibernation_exit_wakeup_window_ms;
            printf(">>> hibernation: topic finished, keep LCD on for wakeup window <<<\r\n");
        }
        break;

    default:
        break;
    }
}

void hibernation_set_timeout_seconds(uint16_t seconds)
{
    if (seconds == 0U) {
        return;
    }

    hibernation_timeout_ms = (uint32_t)seconds * 1000UL;
    hibernation_last_activity_tick = systick_get_tick();
    printf(">>> hibernation timeout set to %u seconds <<<\r\n", seconds);
}

void hibernation_set_wakeup_window_seconds(uint16_t seconds)
{
    if (seconds == 0U) {
        return;
    }

    hibernation_exit_wakeup_window_ms = (uint32_t)seconds * 1000UL;
    printf(">>> hibernation exit-wakeup window set to %u seconds <<<\r\n",
           seconds);
}

static void power_off_sequence(void)
{
    uint16_t voice_id;

    /* Hide the final frame, play the shutdown prompt, then release power. */
    GPIO_ClrBit(GPIOB, PIN6);
    if (hibernation_voice_prng_state == 0) {
        hibernation_voice_prng_state = systick_get_tick() ^ 0x6D2B79F5UL;
    }
    voice_id = POWER_OFF_VOICE_FIRST_ID +
               (hibernation_voice_prng_next() % POWER_OFF_VOICE_COUNT);

    /* Arm the guard before the prompt is sent so every report parsed from here
     * on is judged against the shutdown rules, not the hibernation ones. */
    power_off_audio_done = false;
    power_off_audio_start_tick = systick_get_tick();
    power_off_pending = true;
    send_audio_play_cmd(voice_id);
}

void request_power_off(void)
{
    power_off_requested = true;
}

typedef enum {
    KEY_STATE_IDLE,         // 空闲状态
    KEY_STATE_PRESS,        // 按下状态
    KEY_STATE_LONG_PRESS,   // 长按状态
    KEY_STATE_RELEASE,      // 释放状态
    KEY_STATE_DOUBLE_CLICK  // 双击状态
} KeyState;

// 按键事件定义
typedef enum {
    KEY_EVENT_NONE,
    KEY_EVENT_CLICK,        // 单击事件
    KEY_EVENT_DOUBLE_CLICK, // 双击事件
    KEY_EVENT_LONG_PRESS    // 长按事件
} KeyEvent;

// 按键检测结构体
typedef struct {
    // 配置参数
    int long_press_time;       // 长按触发时间(按键扫描次数)
    int double_click_interval; // 双击间隔时间(按键扫描次数)
    
    // 状态变量
    KeyState state;           // 当前状态
    bool key_pressed;         // 当前按键电平
    bool last_key_pressed;    // 上次按键电平
    
    // 计时相关
    int press_start_time;  // 按下开始时间
    int last_release_time; // 上次释放时间
    
    // 计数
    int click_count;           // 点击计数
    
    // 输出事件
    KeyEvent event;            // 检测到的事件
} KeyDetector;

bool key_level = false;
KeyDetector detector;

// 初始化按键检测器
void KeyDetector_Init(KeyDetector *detector) {
    detector->long_press_time = POWER_OFF_LONG_PRESS_MS / KEY_SCAN_INTERVAL_MS;
    detector->double_click_interval = 20;  // 200ms (20 * 10ms)
    
    detector->state = KEY_STATE_IDLE;
    detector->key_pressed = false;
    detector->last_key_pressed = false;
    detector->press_start_time = 0;
    detector->last_release_time = 0;
    detector->click_count = 0;
    detector->event = KEY_EVENT_NONE;
}

// 按键检测处理函数
KeyEvent KeyDetector_Process(KeyDetector *detector, bool key_level) {
	static int time_cnt = 0;
	time_cnt++;
    int current_time = time_cnt;
    
    // 保存上次按键电平
    detector->last_key_pressed = detector->key_pressed;
    detector->key_pressed = key_level;
    detector->event = KEY_EVENT_NONE;  // 清空事件
    
    // 检测按键下降沿(按下)
    if (detector->key_pressed && !detector->last_key_pressed) {
        // 记录按下时间
        detector->press_start_time = current_time;
        
        // 检查是否可能为双击
        if (detector->click_count == 1 && 
            (current_time - detector->last_release_time) <= detector->double_click_interval) {
            detector->click_count = 2;
        } else {
            detector->click_count = 1;
        }
        
        detector->state = KEY_STATE_PRESS;
    }
    
    // 检测按键上升沿(释放)
    if (!detector->key_pressed && detector->last_key_pressed) {
        // 记录释放时间
        detector->last_release_time = current_time;
        
        // 长按事件已在达到阈值时触发，释放时只复位状态
        if (detector->state == KEY_STATE_LONG_PRESS) {
            detector->state = KEY_STATE_IDLE;
            detector->click_count = 0;  // 清空点击计数
        } 
        // 检查是否是短按
        else if (detector->state == KEY_STATE_PRESS) {
            detector->state = KEY_STATE_RELEASE;
            
            // 等待双击判断
            if (detector->click_count == 2) {
                detector->event = KEY_EVENT_DOUBLE_CLICK;
                detector->click_count = 0;
            }
        }
    }
    
    // 检测长按(按键保持按下状态)
    if (detector->key_pressed && detector->state == KEY_STATE_PRESS) {
        if ((current_time - detector->press_start_time) >= detector->long_press_time) {
            detector->state = KEY_STATE_LONG_PRESS;
            detector->event = KEY_EVENT_LONG_PRESS;
        }
    }
    
    // 超时处理(等待双击超时)
    if (detector->state == KEY_STATE_RELEASE && detector->click_count == 1) {
        if ((current_time - detector->last_release_time) > detector->double_click_interval) {
            // 双击超时，触发单击事件
            detector->event = KEY_EVENT_CLICK;
            detector->click_count = 0;
            detector->state = KEY_STATE_IDLE;
						time_cnt = 0;
        }
    }
    
    // 空闲状态重置
    if (detector->state == KEY_STATE_RELEASE && detector->click_count == 0) {
        detector->state = KEY_STATE_IDLE;
				time_cnt = 0;
    }
    
    return detector->event;
}



void USART0_Handler(void)
{
	uint8_t chr;
	if(USART_INTStat(USART0, USART_IT_RX_RDY))
	{
		chr = USART_Read(USART0);
		android_buf_in(&android_receive_buf,&chr);
	}
	else if(USART_INTStat(USART0, USART_IT_RX_TO))
	{
			USART_INTClr(USART0, USART_IT_RX_TO);
	}
}


void SerialInit(void)
{
	UART_InitStructure UART_initStruct;
	
	PORT_Init(PORTA, PIN0, PORTA_PIN0_UART0_RX, 1);	//GPIOA.0ÅäÖÃÎªUART0 RXD
	PORT_Init(PORTA, PIN1, PORTA_PIN1_UART0_TX, 0);	//GPIOA.1ÅäÖÃÎªUART0 TXD
 	
 	UART_initStruct.Baudrate = 115200;
	UART_initStruct.DataBits = UART_DATA_8BIT;
	UART_initStruct.Parity = UART_PARITY_NONE;
	UART_initStruct.StopBits = UART_STOP_1BIT;
	UART_initStruct.RXThreshold = 3;
	UART_initStruct.RXThresholdIEn = 1;
	UART_initStruct.TXThreshold = 3;
	UART_initStruct.TXThresholdIEn = 0;
	UART_initStruct.TimeoutTime = 10;		//10¸ö×Ö·ûÊ±¼äÄÚÎ´½ÓÊÕµ½ÐÂµÄÊý¾ÝÔò´¥·¢³¬Ê±ÖÐ¶Ï
	UART_initStruct.TimeoutIEn = 1;
 	UART_Init(UART0, &UART_initStruct);
	UART_Open(UART0);
}

void Serial_4G_Init(void)
{
	USART_InitStructure USART_initStruct;
	
	PORT_Init(PORTA, PIN2, PORTA_PIN2_USART0_TX, 0);
	PORT_Init(PORTA, PIN3, PORTA_PIN3_USART0_RX, 1);
 	
	USART_initStruct.Baudrate = 115200;
	USART_initStruct.DataBits = USART_DATA_8BIT;
	USART_initStruct.Parity = USART_PARITY_NONE;
	USART_initStruct.StopBits = USART_STOP_1BIT;
	USART_initStruct.RXReadyIEn = 1;
	USART_initStruct.TXReadyIEn = 0;
	USART_initStruct.TimeoutIEn = 1;
	USART_initStruct.TimeoutTime = 50;	// 50 ¸ö bit Ê±¼äÄÚÎ´½ÓÊÕµ½ÐÂµÄÊý¾Ý£¬´¥·¢½ÓÊÕ³¬Ê±ÖÐ¶Ï
 	USART_Init(USART0, &USART_initStruct);
	USART_Open(USART0);

	GPIO_Init(GPIOA, PIN13, 1, 0, 0, 0);			// 4G power enable
	GPIO_ClrBit(GPIOA, PIN13);
	systick_delay_ms(100);
	GPIO_SetBit(GPIOA, PIN13);
}


void gpio_outhigh(void)
{
	SYS->CLKEN0 |= (0x01 << SYS_CLKEN0_GPIOA_Pos);
	SYS->CLKEN0 |= (0x01 << SYS_CLKEN0_GPIOB_Pos);
	SYS->CLKEN0 |= (0x01 << SYS_CLKEN0_GPIOC_Pos);
	*((volatile uint32_t *)0x40000190) = 0;
	
	GPIO_Init(GPIOA ,1 ,1,0, 0, 0);
	GPIO_Init(GPIOA ,2 ,1,0, 0, 0);
	GPIO_Init(GPIOA ,3 ,1,0, 0, 0);
	GPIO_Init(GPIOA ,4 ,1,0, 0, 0);
	GPIO_Init(GPIOA ,5 ,1,0, 0, 0);
	

	GPIO_Init(GPIOB ,4 ,1,0, 0, 0);
	GPIO_Init(GPIOB ,5 ,1,0, 0, 0);
	GPIO_Init(GPIOB ,6,1,0, 0, 0);
	GPIO_Init(GPIOB ,7,1,0, 0, 0);	
	GPIO_Init(GPIOB ,14,1,0, 0, 0);
	
	GPIO_Init(GPIOC ,0 ,1,0, 0, 0);
	GPIO_Init(GPIOC ,1 ,1,0, 0, 0);
	
	GPIO_SetBit( GPIOA,PIN1); //set gpio 0-gpio15 out high
	GPIO_SetBit( GPIOA,PIN2); //set gpio 0-gpio15 out high
	GPIO_SetBit( GPIOA,PIN3); //set gpio 0-gpio15 out high
	GPIO_SetBit( GPIOA,PIN4); //set gpio 0-gpio15 out high
	GPIO_SetBit( GPIOA,PIN5); //set gpio 0-gpio15 out high
	
	
	GPIO_SetBit(GPIOB,PIN4);
	GPIO_SetBit(GPIOB,PIN5);
	GPIO_ClrBit(GPIOB,PIN6);
	GPIO_SetBit(GPIOB,PIN7);
	GPIO_SetBit(GPIOB,PIN14);

	GPIO_ClrBit(GPIOC,PIN0);
	GPIO_ClrBit(GPIOC,PIN1);
}

int sleep_flag = 0;
int play_sleep_action_flag = 0;//反复播放结束帧标志

enum PLAY_STEP {
ANSWER=0, Spk,Lis,Poweron
}play_eye_step;

bool sc7a20h_ready = false;
int error_cnt=0;
KeyEvent event;

static void main_tick(void) 
{
    display_backlight_startup_task();

    if (power_off_pending) {
        /* Keep draining the CI1303 link; the play-done report arrives here. */
        android_chu_li();

        if (power_off_audio_done ||
            (uint32_t)(systick_get_tick() - power_off_audio_start_tick) >=
                POWER_OFF_AUDIO_TIMEOUT_MS) {
            power_off_pending = false;
            POWER_OFF;
        }
        return;
    }

    if (power_off_requested) {
        power_off_requested = false;
        power_off_sequence();
        return;
    }

    bool local_button_active = local_activity_pending ||
                               (GPIO_GetBit(GPIOA, PIN7) == 0);
    bool local_input_active = local_activity_pending ||
                              (GPIO_GetBit(GPIOB, PIN14) == 0) ||
                              (GPIO_GetBit(GPIOB, PIN15) == 0);
    local_activity_pending = false;

    /* Process CI1303 state/config packets before deciding whether the idle
     * timeout has expired. Capture current_time afterwards because packet
     * handlers may advance hibernation_last_activity_tick. */
    android_chu_li();
    uint32_t current_time = systick_get_tick();

    if (hibernation_last_activity_tick == 0) {
        hibernation_last_activity_tick = current_time;
    }

    if (hibernation_active) {
        /* Keep the existing low-battery protection alive while the display
         * is off and the ESP32/4G rail remains enabled. */
        bat_voltage_task();
        /* The first local key/touch wakes the hardware. It is then handled
         * normally by the existing input state machines on the next tick. */
        if (local_button_active) {
            hibernation_exit();
            hibernation_topic_wake = false;
            hibernation_last_activity_tick = current_time;
        }
        return;
    }

    if (local_input_active) {
        /* Touch and buttons are user activity. A CI1303 conversation state is
         * not: a topic nobody answered reports the same Listening and Speaking
         * as a real turn, so only CI1303_USER_SPEAKING_STATE refreshes the
         * countdown, in the state handler above. */
        hibernation_topic_wake = false;
        hibernation_last_activity_tick = current_time;
    } else if (hibernation_topic_wake) {
        /* A topic owns the panel until this deadline: the reply window once
         * playback has reported done, a generous fallback before that so a lost
         * report cannot strand the panel here. Deliberately not gated on
         * ci1303_voice_active, which only moves when a report arrives. */
        if ((int32_t)(current_time - hibernation_topic_wake_deadline) >= 0) {
            if ((uint32_t)(current_time - hibernation_last_activity_tick) >=
                    hibernation_timeout_ms) {
                /* Nobody answered and nobody was here. Go quiet without
                 * replaying a hibernation prompt that already played. */
                hibernation_reenter_quiet();
                return;
            }
            /* Someone was active recently, so the topic only borrowed a panel
             * the ordinary countdown still owns. Hand it back and let that
             * countdown expire normally, prompt included. */
            hibernation_topic_wake = false;
            hibernation_topic_wake_deadline = 0;
        }
    } else if (!ci1303_voice_active &&
               (uint32_t)(current_time - hibernation_last_activity_tick) >=
                   hibernation_timeout_ms) {
        /* Never blank mid-topic or inside an open wakeup window. The countdown
         * still runs underneath; the topic deadline closes the panel. */
        hibernation_enter();
        return;
    }

    // servo_run_tick(); // Disabled, servo pins NC or reused
    bat_voltage_task();
    face_timeout_task();

    // Servo testing logic removed. Use servo_set_angle() to control.
    
    static uint32_t last_tick_time = 0;
    uint32_t delta_ms = 10;
    if (last_tick_time != 0) {
        delta_ms = current_time - last_tick_time;
        if (delta_ms > 1000) delta_ms = 10; // Cap at 1s in case of huge hangs
    }
    last_tick_time = current_time;

    // Touch Logic Variables (Touch 1)
    static uint32_t touch1_time = 0;
    static uint8_t touch1_clicks = 0;
    static uint32_t touch1_release_wait_time = 0;
    static uint32_t touch1_off_time = 0;
    static uint32_t touch1_long_press_time = 0;
    static uint8_t touch1_long_press_triggered = 0;
    static uint32_t touch1_idle_time = 0;

    // Touch Logic Variables (Touch 2)
    static uint32_t touch2_time = 0;
    static uint8_t touch2_clicks = 0;
    static uint32_t touch2_release_wait_time = 0;
    static uint32_t touch2_off_time = 0;
    static uint32_t touch2_long_press_time = 0;
    static uint8_t touch2_long_press_triggered = 0;
    static uint32_t touch2_idle_time = 0;
    
    extern void trigger_face_by_id(uint16_t face_id);

    /* Touch gestures send local-audio commands to CI1303. Do not let a touch
     * preempt an active cloud conversation; discard the entire gesture so it
     * cannot be delivered later when listening or speaking ends. */
    if (ci1303_voice_active) {
        touch1_time = 0;
        touch1_clicks = 0;
        touch1_release_wait_time = 0;
        touch1_off_time = 0;
        touch1_long_press_time = 0;
        touch1_long_press_triggered = 0;
        touch1_idle_time = 0;
        touch2_time = 0;
        touch2_clicks = 0;
        touch2_release_wait_time = 0;
        touch2_off_time = 0;
        touch2_long_press_time = 0;
        touch2_long_press_triggered = 0;
        touch2_idle_time = 0;
    } else {
    // Touch Logic 1 (PB14)
    if (GPIO_GetBit(GPIOB, PIN14) == 0) {
        touch1_off_time = 0;
        if (touch1_time == 0) {
            printf("Touch 1: Detected LOW (Pressed)\r\n");
        }
        touch1_time += delta_ms;
        touch1_release_wait_time = 0;
        
        touch1_idle_time = 0;
        if (!touch1_long_press_triggered) {
            touch1_long_press_time += delta_ms;
            if (touch1_long_press_time >= 3000) { // 3 seconds real time
                printf("Touch 1: Long Press triggered! (3 seconds)\r\n");
                trigger_face_by_id(403);
                touch1_long_press_triggered = 1;
                touch1_clicks = 0;
            }
        }
    } else {
        if (touch1_time > 0) {
            touch1_off_time += delta_ms;
            if (touch1_off_time <= 150) { // 150ms gap bridge
                touch1_time += delta_ms;
                if (!touch1_long_press_triggered) {
                    touch1_long_press_time += delta_ms;
                    if (touch1_long_press_time >= 3000) {
                        printf("Touch 1: Long Press triggered! (3 seconds)\r\n");
                        trigger_face_by_id(403);
                        touch1_long_press_triggered = 1;
                        touch1_clicks = 0;
                    }
                }
            } else {
                printf("Touch 1: Released. Held for %d ms.\r\n", touch1_time);
                if (touch1_time > 50 && !touch1_long_press_triggered) { // Valid click
                    touch1_clicks++;
                    touch1_release_wait_time = 1;
                    printf("Touch 1: Click count is now %d\r\n", touch1_clicks);
                }
                touch1_time = 0;
                touch1_off_time = 0;
            }
        }
        
        touch1_idle_time += delta_ms;
        if (touch1_idle_time > 1500) { // 1.5 seconds of absolutely no touch
            touch1_long_press_time = 0;
            touch1_long_press_triggered = 0;
        }
        
        if (touch1_time == 0 && touch1_release_wait_time > 0) {
            touch1_release_wait_time += delta_ms;
            if (touch1_release_wait_time > 600) { // 600ms timeout for next click
                if (!touch1_long_press_triggered) {
                    printf("Touch 1: Click timeout reached. Total clicks: %d\r\n", touch1_clicks);
                    if (touch1_clicks == 1) {
                        printf("Touch 1: Executing Single Click (Screen 401)\r\n");
                        trigger_face_by_id(401);
                    } else if (touch1_clicks >= 2) {
                        printf("Touch 1: Executing Double Click (Screen 402)\r\n");
                        trigger_face_by_id(402);
                    }
                }
                touch1_clicks = 0;
                touch1_release_wait_time = 0;
            }
        }
    }

    // Touch Logic 2 (PB15)
    if (GPIO_GetBit(GPIOB, PIN15) == 0) {
        touch2_off_time = 0;
        if (touch2_time == 0) {
            printf("Touch 2: Detected LOW (Pressed)\r\n");
        }
        touch2_time += delta_ms;
        touch2_release_wait_time = 0;
        
        touch2_idle_time = 0;
        if (!touch2_long_press_triggered) {
            touch2_long_press_time += delta_ms;
            if (touch2_long_press_time >= 3000) {
                printf("Touch 2: Long Press triggered! (3 seconds)\r\n");
                trigger_face_by_id(403);
                touch2_long_press_triggered = 1;
                touch2_clicks = 0;
            }
        }
    } else {
        if (touch2_time > 0) {
            touch2_off_time += delta_ms;
            if (touch2_off_time <= 150) {
                touch2_time += delta_ms;
                if (!touch2_long_press_triggered) {
                    touch2_long_press_time += delta_ms;
                    if (touch2_long_press_time >= 3000) {
                        printf("Touch 2: Long Press triggered! (3 seconds)\r\n");
                        trigger_face_by_id(403);
                        touch2_long_press_triggered = 1;
                        touch2_clicks = 0;
                    }
                }
            } else {
                printf("Touch 2: Released. Held for %d ms.\r\n", touch2_time);
                if (touch2_time > 50 && !touch2_long_press_triggered) {
                    touch2_clicks++;
                    touch2_release_wait_time = 1;
                    printf("Touch 2: Click count is now %d\r\n", touch2_clicks);
                }
                touch2_time = 0;
                touch2_off_time = 0;
            }
        }
        
        touch2_idle_time += delta_ms;
        if (touch2_idle_time > 1500) {
            touch2_long_press_time = 0;
            touch2_long_press_triggered = 0;
        }
        
        if (touch2_time == 0 && touch2_release_wait_time > 0) {
            touch2_release_wait_time += delta_ms;
            if (touch2_release_wait_time > 600) {
                if (!touch2_long_press_triggered) {
                    printf("Touch 2: Click timeout reached. Total clicks: %d\r\n", touch2_clicks);
                    if (touch2_clicks == 1) {
                        printf("Touch 2: Executing Single Click (Screen 301_404)\r\n");
                        trigger_face_by_id(404);
                    } else if (touch2_clicks >= 2) {
                        printf("Touch 2: Executing Double Click (Screen 307)\r\n");
                        trigger_face_by_id(405);
                    }
                }
                touch2_clicks = 0;
                touch2_release_wait_time = 0;
            }
        }
    }

    }

//		if(GPIO_GetBit(GPIOB, PIN14) ==0)	//touch1
//				{
//						device.evt.sta_now = 0x06;
//				}
//				
//		if(GPIO_GetBit(GPIOA, PIN3) == 0)		//switch power
//		{
//			 key_level = true;
//		}
//		else
//		{
//			key_level = false;
//		}
			
    if(sc7a20h_ready) {
        Check_SC7A20H_State(acc_task(&SC7A20H,&Counter));
    }

    /* 
     * TODO: The old hardcoded SCREEN001/SCREEN002 logic has been disabled because it 
     * conflicts with the new 15-group animation mapping in Analysis.c.
     * You can implement your continuous state-machine logic (Idle, Speaking, Listening) here,
     * ensuring you use the correct SCREEN00X IDs corresponding to your new animations!
     */
    /*
    if(device.Module_sta==0x06) {
        if(play_eye_step != Spk) {
            finish_play_flag = 1;
            synwit_ug_load_screen(SCREEN001);
            play_eye_step = Spk;
        }
        else if(finish_play_flag == 0) {
            finish_play_flag = 1;
            synwit_ug_load_screen(SCREEN002);
        }
    } 
    else if(device.Module_sta==0x04) {
        if(play_eye_step != Lis) {
            finish_play_flag = 1;
            synwit_ug_load_screen(SCREEN002);
            play_eye_step = Lis;
        }
    }
    else {
        play_eye_step=ANSWER;
        if(finish_play_flag==0) {
            finish_play_flag = 1;
            synwit_ug_load_screen(SCREEN002);
        }
    }
    */

	send_msg();	
}


void BTIMR0_Handler(void)
{
	TIMR_INTClr(BTIMR0);
				// Touch is now handled in main_tick() using a state machine
				// if(GPIO_GetBit(GPIOB, PIN9) == 0)	//touch1 (PB9)
				// {
				// 		device.evt.sta_now = 0x06;
				// }
				// else if(GPIO_GetBit(GPIOA, PIN12) == 0) //touch2 (PA12)
				// {
				// 		device.evt.sta_now = 0x06;
				// }
//				else if(evt==12)
//				{
//					printf("EVT_SHAKE_FORWARDBACK Y\n");
//					device.evt.sta_now = 0x01;
//				}
				
		if(GPIO_GetBit(GPIOA, PIN7) == 0)		//switch power
		{
			 key_level = true;
			 local_activity_pending = true;
		}
		else
		{
			key_level = false;
		}

	 event = KeyDetector_Process(&detector, key_level);
				switch (event) {
            case KEY_EVENT_CLICK:
                printf(">>> click <<<\r\n");
                extern void send_audio_play_cmd(uint16_t voice_id);
                send_audio_play_cmd(301);
                break;
            case KEY_EVENT_DOUBLE_CLICK:
                printf(">>> double_click <<<\r\n");
                break;
            case KEY_EVENT_LONG_PRESS:
                printf(">>> power_off_long_press_3s <<<\r\n");
                request_power_off();
                break;
            default:
                break;
        }
}


static const SYS_OPS sys_ops = {
    .disp.area_set = area_set,
    .disp.flush_pixels = flush_pixels,
    .disp.flush_start = flush_start,
    .disp.flush_ready = flush_ready,
    .data.ui_data_read = ui_data_read,
    
    .app.framework_ready = framework_ready,
    .app.app_ready = app_ready,
    .app.pre_blue = pre_blue,
    
    .app.main_tick = main_tick,// main_tick, /* 主循环每跑一次，都会触发一次这个函数 */
    .app.main_tick_period = 10, /* 指定 main_tick 回调函数的间隔，毫秒为单位，默认为 50ms */

    /* 串口屏功能 */
#if (ENABLED_SERIAL_DISPLAY == 1)
    .sdisp.rx_handler = sdisp_handler,
    .sdisp.notify = sdisp_notify,
#else
    .sdisp.rx_handler = NULL,
    .sdisp.notify = NULL,
#endif
};

static const SYS_CONFIG sys_conf = {
    .display_width = DISP_WIDTH,   /* 屏幕宽度 */
    .display_height = DISP_HEIGHT, /* 屏幕高度 */

    /* 分配给每个显示缓存的像素个数(0表示取屏幕宽度) */
    .pixels_per_pfb = DISP_PFB_PIXELS,
    /* 显示缓存个数(可以是1: sync 或 2: async)*/
    .num_of_pfb = 1,

    .heap = platform_heap,             /* 内存堆地址 */
    .heap_len = sizeof(platform_heap), /* 内存堆大小(字节单位) */

    /* 刷新区域的对齐限定。如果屏或LCD控制器无特别限制，一般设为0即可。
     * 可以设为以下一个或多个限定条件(用|运算符)：
     *
     * [偶数开窗可选配置项]
     *      FAA_X2:         限定刷新区的x为2的倍数
     *      FAA_Y2:         限定刷新区的y为2的倍数
     *      FAA_W2:         限定刷新区的宽度为2的倍数
     *      FAA_H2:         限定刷新区的高度为2的倍数
     *
     * [4整数倍开窗可选配置项]
     *      FAA_X4:         限定刷新区的x为4的倍数
     *      FAA_W4:         限定刷新区的宽度为4的倍数
     * 
     */
    .flush_area_alignment = 0,
};

void systick_handler_hook(void)
{
    UG_TickInc(1);
    // insert user code(note: It is Systick ISR)

    __IMPORT_TEST_CASE(systick_handler);
}


int main(void)
{
	/* Enable GPIOA clock to initialize PA13 immediately and prevent 4G module from glitching/floating */
	SYS->CLKEN0 |= (0x01 << SYS_CLKEN0_GPIOA_Pos);
	GPIO_Init(GPIOA, PIN13, 1, 0, 0, 0);			// 4G power pin as output
	GPIO_ClrBit(GPIOA, PIN13);						// Pull LOW to keep 4G off during boot

	GPIO_Init(GPIOA, PIN5, 0, 1, 0, 0);						//厂测引脚
    
	/* 在调测阶段(版本 Release 时可注释), 预防程序跑飞后, 上电瞬间锁死内核导致无法通过 SWD 访问, 也可用作等待个别硬件模块上电稳定 */
    // for (uint32_t i = 0; i < 111111 *1; ++i) __NOP();		//O3/Ofast优化无意义空循环
    for (uint32_t i = 0; i < 111111 *1; ++i) __NOP();
		board_init();
		/* Hold the LCD backlight off before the panel driver starts. This avoids
		 * a floating/high PB6 exposing ST7789 GRAM during initialization. */
		GPIO_Init(GPIOB, PIN6, 1, 0, 0, 0);
		GPIO_ClrBit(GPIOB, PIN6);
		bat_monitor_init();     // Initialize battery monitor
		KeyDetector_Init(&detector);
		GPIO_Init(GPIOA, PIN7, 0, 1, 0, 0);			//KEY 1
		GPIO_Init(GPIOA, PIN6, 1, 0, 0, 0);			//KEY 2

        // Latch power immediately
        POWER_ON;

        // Initialize LCD driver immediately to turn on screen
        driver_init();

        // Wait for the user to release the boot click before starting the key detector
        while (GPIO_GetBit(GPIOA, PIN7) == 0) {
            __NOP();
        }

        // Start timers and initialize other peripherals
		TIMR_Init(BTIMR0, TIMR_MODE_TIMER, CyclesPerUs, 10000, 1);
		TIMR_Start(BTIMR0);
        GPIO_Init(GPIOB, PIN14, 0, 1, 0, 0);			//touch 1 (PB14) - Pull-up, active low
        GPIO_Init(GPIOB, PIN15, 0, 1, 0, 0);		//touch 2 (PB15) - Pull-up, active low
		// servo_init(); // Disabled, servo pins NC or reused
		Serial_4G_Init();
		// Sc7a20h_i2c_Init();
	    // for (int i = 0; i < 2; i++) {
        // err_t err = SL_Sc7a20h_Config();
        // if (err == SWM_OK) {
        //     sc7a20h_ready = true;
        //     break;
        // }
    // }
			
		play_eye_step = Poweron;
    char ver[48];
    synwit_ug_get_platform_version_name(ver, sizeof(ver));	//get gui information
    printf("\r\n-------------------------------------\r\n");
    printf("[%s] port [%s] platform version: %s Demo \r\n", "SWM221", "ugui", ver);
    printf("build-release %s-%s \r\n", __DATE__, __TIME__);
    printf("@Copyright by Synwit Technology");
    printf("\r\n-------------------------------------\r\n");

		printf("gui init over\r\n");
    synwit_ug_start(&sys_ops, &sys_conf);	//start framework
    /* Should not reach here as the scheduler is already started. */

    for (;;) {
        __NOP();
    }
    return 0;
}

static void driver_init(void)
{
    const lcd_mpu_cfg_t lcd_cfg = {
        .name = CFG_LCD_NAME,
        .hres = CFG_LCD_HDOT,
        .vres = CFG_LCD_VDOT,
        .interface = CFG_LCD_IF,
    };
		// printf("Step 11\r\n");
    if (0 != lcd_mpu_init(This_LCD, &lcd_cfg)) {
        printf("[%s]: [lcd_mpu_init] config error!\r\n", __FUNCTION__);
        while (1) __NOP(); /* capture LCD driver error! */
    } else {
		// printf("Step 2\r\n");
	}
    /* 若屏内 COG 驱动芯片支持横竖屏转换(对调分辨率),
     * 当用户有需求时可手动在 LCD 初始化后显式调用 lcd_mpu_set_rotate() 接口进行旋转,
     * 旋转方向请参考具体驱动 lcd_xxx.c 实现;
     */
    // lcd_mpu_set_rotate(This_LCD, 0);
    /* st7789_timseq_init() already wakes and enables the panel. Do not call
     * power mode 1 here because it also turns PB6 backlight on before the UI
     * has rendered its first stable frame. */
    GPIO_ClrBit(GPIOB, PIN6);
    // lcd_mpu_set_backlight(This_LCD, 100); // 开光
    qspi_multiplex_flash(); // QSPI 分时复用 Flash 
    /* SPI_Flash */
    qspi_flash_init();
    qspi_multiplex_lcd(); // QSPI 分时复用 LCD 
	qspi_flash_release_deep_power_down();				//特别重要! 防止在休眠状态下断电后,flash还是休眠状态，加载不了图片数据
    __IMPORT_TEST_CASE(driver_init);
}

void HardFault_Handler(void)
{
    printf("[%s]!\r\n", __FUNCTION__);
    while (1) __NOP();
}
