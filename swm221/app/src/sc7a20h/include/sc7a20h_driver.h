#ifndef SC7A20H_DRIVER_H
#define SC7A20H_DRIVER_H

#include "board.h"
#include "sc7a20h_user.h"


// ----------------- Define -----------------
#define SC7A20H_ADDR        0x18

// ------------------ Groble ------------------
extern char mst_txbuff[1];
extern char mst_rxbuff[4];
extern uint32_t SLV_ADDR;


// ------------------ Groble ------------------
err_t Sc7a20h_i2c_Init();

void I2C_Test_Demo(void);

err_t SL_Sc7a20h_Soft_Reset(void);

err_t SL_Sc7a20h_Config(void);

// read XYZ_DATA register.
err_t sc7a20h_read_accel(Acc_Struct *sc7a20h);

#ifdef __cplusplus
extern "C" {
#endif

#ifdef __cplusplus
}
#endif

#endif
