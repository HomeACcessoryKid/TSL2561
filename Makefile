PROGRAM=main
EXTRA_COMPONENTS = \
	extras/http-parser \
	extras/rboot-ota \
    extras/i2c \
    extras/tsl2561 \
    extras/paho_mqtt_c \
	$(abspath esp-wolfssl) \
	$(abspath esp-cjson) \
	$(abspath esp-homekit) \
	$(abspath UDPlogger) \

FLASH_SIZE ?= 8
HOMEKIT_SPI_FLASH_BASE_ADDR ?= 0x8C000

EXTRA_CFLAGS += -I../.. -DHOMEKIT_SHORT_APPLE_UUIDS

SCL_PIN ?= 2
SDA_PIN ?= 0
I2C_BUS ?= 0
EXTRA_CFLAGS += -DSCL_PIN=$(SCL_PIN) -DSDA_PIN=$(SDA_PIN) -DI2C_BUS=$(I2C_BUS)

ifdef VERSION
EXTRA_CFLAGS += -DVERSION=\"$(VERSION)\"
endif

EXTRA_CFLAGS += -DUDPLOG_PRINTF_TO_UDP
EXTRA_CFLAGS += -DUDPLOG_PRINTF_ALSO_SERIAL

include $(SDK_PATH)/common.mk

monitor:
	$(FILTEROUTPUT) --port $(ESPPORT) --baud $(ESPBAUD) --elf $(PROGRAM_OUT)

sig:
	openssl sha384 -binary -out firmware/main.bin.sig firmware/main.bin
	printf "%08x" `cat firmware/main.bin | wc -c`| xxd -r -p >>firmware/main.bin.sig
	ls -l firmware
