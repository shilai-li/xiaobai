.PHONY: init
init:
	@echo "Initializing environment..."
	mkdir -p ~/esp && cd ~/esp
	@echo "Cloning esp-idf repository..."
	git clone -b v5.5.2 --recursive https://github.com/espressif/esp-idf.git
	@echo "Installing esp-idf for esp32c3..."
	cd esp-idf && ./install.sh esp32c3
	@echo "ESP-IDF for esp32c3 installed successfully."

.PHONY: build
build:
	@echo "Preparing environment..."
	. ~/esp/esp-idf/export.sh && echo "Building project..." && python scripts/release.py esp32c3-ci130x
	@echo "Project built successfully."

.PHONY: flash
flash:
	@echo "Flashing project..."
	. ~/esp/esp-idf/export.sh && echo "Flashing project..." && idf.py -p /dev/cu.usbmodem13301 flash monitor
	@echo "Project flashed successfully."
