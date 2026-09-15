#include "SWM221.h"
#include "dev_qspi_flash.h"

/* 注意：EnterSleepMode() 必须在RAM中执行，Keil下实现方法有：
   方法一、Scatter file
   方法二、code_in_ra.c上右键 =》Options for File "SWM221_sleep.c" =》Properties =》Memory Assignment =》Code/Conts 选择 IRAM1
*/
__attribute__((noinline, used, section(".SRAM")))\
void EnterSleepMode(void) 
{ 
		volatile int i;
	
		SYS->SLEEP |= (1 << SYS_SLEEP_SLEEP_Pos);
			
		while((SYS->PAWKSR & (1 << PIN0)) == 0)
		{
				__NOP();
		}
		SYS->PAWKSR |= (1 << PIN0);							//清除唤醒状态

		qspi_flash_release_deep_power_down();
		for(i=0;i<1000;i++)//等待FLASH唤醒,至少需要20uS.
		{
			__NOP();__NOP();__NOP();
		}
}  
