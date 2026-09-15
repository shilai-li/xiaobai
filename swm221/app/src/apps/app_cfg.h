/**
 *******************************************************************************************************************************************
 * @file        app_cfg.h
 * @brief       app global config
 * @since       Change Logs:
 * Date         Author       Notes
 * 2024-02-18   lzh          the first version
 * @note        ��ʾ������ Synwit �ٷ��� SWM221 ϵ�� DEMO ������ʾ��,
 * ���Ƕ��ֹ��� TFT-LCD (MPU/MCU �ӿ�) ����Ӧ��, ���������õ� CTP ����,
 * �������ֱ�ʾ�����ݵ�����Ļ��ʾ�ķ�ʽ: 
 * 1������ʾ��ö�ٵ���������Ļ�ͺ�, ��Ļģ�����ϵ Synwit �ٷ�/�����̻�ȡ�������ӻ��ϵ���ƽ̨����.
 * 2�����û���������һ�鲻�ڱ�ʾ��ö�ٵ��������ͺ��ڵ���Ļ, �������ڱ�ʾ�������Ӳ����Ӧ TFT-LCD ģ��� ��ʾ/���� ����������.
 * @remark      ���������ϵ���Ļ�廨����, ������ʱ��, ���û�ʹ�õ� TFT-LCD ģ�鲻��ԭ���ѵ��Թ����ͺŷ�Χ��, 
 * ������ȷ���⼸��: ��Ļ֧�ֵĽӿ�ʱ��FPC ������򡢷ֱ��ʹ��.
 *******************************************************************************************************************************************
 * @attention   THE PRESENT FIRMWARE WHICH IS FOR GUIDANCE ONLY AIMS AT PROVIDING CUSTOMERS WITH CODING INFORMATION
 * REGARDING THEIR PRODUCTS IN ORDER FOR THEM TO SAVE TIME. AS A RESULT, SYNWIT SHALL NOT BE HELD LIABLE
 * FOR ANY DIRECT, INDIRECT OR CONSEQUENTIAL DAMAGES WITH RESPECT TO ANY CLAIMS ARISING FROM THE CONTENT
 * OF SUCH FIRMWARE AND/OR THE USE MADE BY CUSTOMERS OF THE CODING INFORMATION CONTAINED HEREIN IN CONN-
 * -ECTION WITH THEIR PRODUCTS.
 * @copyright   2012 Synwit Technology
 *******************************************************************************************************************************************
 */
#ifndef __APP_CFG_H__
#define __APP_CFG_H__
// <<< Use Configuration Wizard in Context Menu >>> 
/* How many bytes of storage are needed for 1 pixel */
#define CFG_LCD_DEPTH_RGB232              ( 1 ) /**< 8 bit(reserve, monochrome 1 => 8) */
#define CFG_LCD_DEPTH_RGB565              ( 2 ) /**< RGB565 = 16 bit */
#define CFG_LCD_DEPTH_RGB888              ( 3 ) /**< RGB888 = 24 bit(reserve) */
#define CFG_LCD_DEPTH_ARGB888             ( 4 ) /**< ARGB888 = 32 bit(reserve) */

#define CFG_LCD_IF_QSPI                   ( 0 ) /**< [Serial] Quad SPI, support DMA(Asnyc / Sync) */
#define CFG_LCD_IF_I8080_8                ( 1 ) /**< [Parallel] Hardware LCDC, support DMA(Asnyc / Sync) */
#define CFG_LCD_IF_SPI_8                  ( 2 ) /**< [Serial] line-4, support DMA(Asnyc / Sync) */
#define CFG_LCD_IF_SPI_9                  ( 3 ) /**< [Serial] line-3, Not recommend, it is slow */

// <h> LCD Configuration
// <i> �����ʵ��ʹ�� TFT-LCD ģ���ͺ���д��Ӧ��Ϣ
// <i> ��ʹ�ñ�ʾ��δ��������ͺ�, �û����������Ӳ����Ӧ��������������������.
//   <s> LCD name (���ִ�Сд)
//   <i> Note:
//   <i> ������ COG ����оƬ֧�ֺ�����ת��(�Ե��ֱ���), ���û�������ʱ���ֶ��� LCD ��ʼ������ʽ���� lcd_mpu_set_rotate() �ӿڽ�����ת, ��ת������ο��������� lcd_xxx.c ʵ��;
//   <i> ZZW180WBS   |  1.8 inch [360*360](ST77916), ��������QSPI��
//   <i> H043A28     |  4.3 inch [480*272](NV3041), �κ�̩��QSPI / i8080_8bit / SPI-4/3��
//   <i> ZJY240IT008 |  2.4 inch [240*320](ST7789), �о�԰��i8080_16/8bit / SPI-4/3��
//   <o> LCD width pixel <0-480>
//   <i> LCD �ֱ���-��(0~480]
//   <o1> LCD height pixel <0-480>
//   <i> LCD �ֱ���-��(0~480]
//   <o2> LCD color depth <2=> RGB565  
//   <i> RGB565: default
//   <o3> LCD interface <0=> QSPI  <1=> i8080 8bit  <2=> spi 8bit  <3=> spi 9bit
//   <i> Quad_SPI    : [Serial] Quad SPI, support DMA(Asnyc / Sync), note: �����Դ� GRAM
//   <i> i8080 8bit  : [Parallel] Hardware LCDC, support DMA(Asnyc / Sync)
//   <i> spi 8bit    : [Serial] line-4, support DMA(Asnyc / Sync)
//   <i> spi 9bit    : [Serial] line-3, Not recommend, it is slow
// </h>
#define CFG_LCD_NAME                       "ZZW180WBS"
#define CFG_LCD_HDOT                       ( 128 )      //160原本
#define CFG_LCD_VDOT                       ( 160 )      //128原本
#define CFG_LCD_DEPTH                      ( 2 ) //(reserve unused)ͨ�� RGB565 ��С�ߴ����ϵ�ɫ�ʱ������㹻�������Ӧ�ó���
#define CFG_LCD_IF                         ( 2 ) //interface

// <h> TP Configuration
// <i> �����ʵ��ʹ�� TFT-LCD ģ���ͺ���д��Ӧ��Ϣ
// <i> ��ʹ�ñ�ʾ��δ��������ͺ�, �û����������Ӳ����Ӧ��������������������.
//   <s> TP name (���ִ�Сд)
//   <i> CHSC6413         |  ˼��΢ ϵ��ģ��
//   <i> CHSC6540         |  �����ӽ� ϵ��ģ��
//   <i> GT911            |  �κ�̩ H043A28
//   <i> FT6336           |  No instances available
//   <i> CST826           |  No instances available
//   <i> CST816D          |  ������ ZZW180WBS
//   <i> FT5206           |  No instances available
//   <i> CST328           |  No instances available
//   <i> Tango_C32        |  No instances available
//   <i> ILI2117A         |  No instances available
#define CFG_TP_NAME                        "CST816D" //����������� TP ����Ӧ��ĳЩ��Ļ��������
//   <!c1> TP enable
//   <i> �Ƿ����� TP (default: true)
#undef CFG_TP_NAME
//   </c>
// </h>
// <<< end of configuration section >>> 
#if !(defined(CFG_LCD_NAME)   \
    && defined(CFG_LCD_HDOT)  \
    && defined(CFG_LCD_VDOT)  \
    && defined(CFG_LCD_DEPTH) \
    && defined(CFG_LCD_IF))
# error "Please define the [CFG_LCD_NAME / CFG_LCD_HDOT / CFG_LCD_VDOT / CFG_LCD_DEPTH / CFG_LCD_IF]"
#endif
//#ifndef CFG_TP_NAME
//# warning "No define the [CFG_TP_NAME], You cannot use touch input device!"
//#endif
#endif //__APP_CFG_H__
