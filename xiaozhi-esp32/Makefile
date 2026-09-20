.PHONY: init
init:
	@echo "Initializing environment..."
	mkdir -p ~/esp && cd ~/esp  && git clone -b v5.5.4 --recursive https://github.com/espressif/esp-idf.git && cd esp-idf && ./install.sh esp32c3
	@echo "ESP-IDF for esp32c3 installed successfully."

.PHONY: esp32c3
esp32c3:
	@echo "Setting up esp32c3 environment..."
	. ~/esp/esp-idf/export.sh && idf.py set-target esp32c3
.PHONY: clean
clean:
	@echo "Cleaning environment..."
	. ~/esp/esp-idf/export.sh && rm -rf ~/.cache && rm -rf ./build && idf.py fullclean && rm -rf ./releases
	@echo "Environment cleaned successfully."

.PHONY: build
build:
	@echo "Preparing environment..."
	. ~/esp/esp-idf/export.sh && echo "Building project..." && python scripts/release.py esp32c3-ci130x
	@echo "Project built successfully."

PORT ?= /dev/cu.usbmodem13301

.PHONY: flash
flash:
	@echo "Flashing project..."
	. ~/esp/esp-idf/export.sh && echo "Flashing project..." && idf.py -p $(PORT) flash monitor
	@echo "Project flashed successfully."

LN ?= plog1
.PHONY: log
log:
	picocom -b 115200 --logfile $(LN).log $(PORT)