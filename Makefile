obj-m += hid-hyperx.o

KDIR ?= /lib/modules/$(shell uname -r)/build
BUILD_DIR := $(CURDIR)/build

.PHONY: all modules clean compile_commands

all: modules compile_commands

modules:
	$(MAKE) -C $(KDIR) M=$(CURDIR) MO=$(BUILD_DIR) modules

compile_commands:
	@mkdir -p $(BUILD_DIR)
	@command -v bear >/dev/null 2>&1 || { \
		echo "Error: bear is required to generate compile_commands.json"; \
		echo "Install it with: sudo apt install bear"; \
		exit 1; \
	}
	@cd $(BUILD_DIR) && bear -- \
		$(MAKE) -C $(KDIR) M=$(CURDIR) MO=$(BUILD_DIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) MO=$(BUILD_DIR) clean
	@rm -rf $(BUILD_DIR)
