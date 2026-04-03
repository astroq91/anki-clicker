.PHONY: build flash

all: build

build:
	 west build -b nrf52840dk/nrf52840

flash: build
	openocd -f interface/cmsis-dap.cfg -f target/nrf52.cfg -c "program build/zephyr/zephyr.elf verify reset exit"
