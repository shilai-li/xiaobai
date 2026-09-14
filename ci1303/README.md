# CI1303 Firmware

This directory contains the CI1303 firmware project for the ESP32-C3 board.

## Required SDK

The project requires **CI13XX_SDK_LLM_AIOT_2.1.2**. The SDK is not included in
this repository because it provides the CI1303 toolchain, headers, libraries,
linker files, build utilities, and voice-model support.

Obtain and extract the SDK locally, for example:

```text
C:/work/CI13XX_SDK_LLM_AIOT_2.1.2/
```

## Build

Build the project from its `project_file` directory using GNU Make and the
toolchain supplied with the SDK:

```bash
cd ci1303/esp32c3_8m_ci1303_4m_swm221cbt7_8m/project_file
make SDK_PATH=/path/to/CI13XX_SDK_LLM_AIOT_2.1.2
```

On Windows, use the SDK path in the shell's format, for example:

```bash
make SDK_PATH=/c/work/CI13XX_SDK_LLM_AIOT_2.1.2
```

The default `SDK_PATH` in the makefile assumes that this project is located
inside the SDK's project tree. When using this standalone repository, pass
`SDK_PATH` explicitly as shown above.

## Build output

Generated files are written under:

```text
ci1303/esp32c3_8m_ci1303_4m_swm221cbt7_8m/project_file/build/
ci1303/esp32c3_8m_ci1303_4m_swm221cbt7_8m/firmware/user_code/
```

To remove intermediate build files:

```bash
make SDK_PATH=/path/to/CI13XX_SDK_LLM_AIOT_2.1.2 clean
```

Use the flashing and firmware-packaging tools included with
`CI13XX_SDK_LLM_AIOT_2.1.2` to program the generated firmware onto the board.
