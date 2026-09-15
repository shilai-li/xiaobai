@echo off

SET dir=src\apps\ui_src\appkit
SET app_c=%dir%\app.c
SET app_h=%dir%\app.h
SET data_dir=%dir%\data
SET data_c=%data_dir%\ui_data00.c

if not exist %app_c% (
	echo "Creating default app.c"
	md %dir%
	cd.>%app_c%
	echo #include "app.h">>%app_c%
	echo.>>%app_c%
)

if not exist %app_h% (
	echo "Creating default app.h"
	md %dir%
	cd.>%app_h%
	echo #ifndef SYNWIT_UG_APP_H>>%app_h%
	echo #define SYNWIT_UG_APP_H>>%app_h%
	echo.>>%app_h%
	echo #define ENABLED_SERIAL_DISPLAY	1 >>%app_h%
	echo.>>%app_h%
	echo #endif>>%app_h%
	echo.>>%app_h%
)

if not exist %data_c% (
	echo "Creating default ui_data.c"
	md %data_dir%

	echo const __attribute__^(^(aligned^(4^)^)^) __attribute__^(^(section^(".SPIFLASH0"^)^)^) unsigned char ui_data0[] = {0xff};>%data_dir%\ui_data00.c
	cd.>%data_dir%\ui_data01.c
	cd.>%data_dir%\ui_data02.c
	cd.>%data_dir%\ui_data03.c
	cd.>%data_dir%\ui_data04.c
	cd.>%data_dir%\ui_data05.c
	cd.>%data_dir%\ui_data06.c
	cd.>%data_dir%\ui_data07.c
)