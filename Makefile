PI_HOST ? raspberrypi.local
PI_USER ? pi
PI_REMOTE := $(PI_USER)@$(PI_HOST)
PI_DIR    := ~/camera

.PHONY: all build clean flash ssh deploy remote-build remote-run remote-clean

all: build

build:
	mkdir -p build && cd build && cmake .. && make -j$$(nproc)

clean:
	rm -rf build

flash:
	@echo "=== Flash Pi OS via USB (rpiboot) ==="
	@echo "1. Insert microSD, connect Pi via USB (BCM2710 Boot)"
	@echo "2. sudo rpiboot"
	@echo "3. Pi appears as USB mass storage — flash OS:"
	@echo "   xzcat raspios-lite-arm64.img.xz | sudo dd of=/dev/sdX bs=4M status=progress"
	@echo "4. Mount the boot partition and configure:"
	@echo "   sudo mount /dev/sdX1 /mnt"
	@echo "   sudo touch /mnt/ssh"
	@echo "   sudo cp config/wpa_supplicant.conf.example /mnt/wpa_supplicant.conf"
	@echo "      (edit /mnt/wpa_supplicant.conf with your WiFi credentials first)"
	@echo "   echo 'dtoverlay=imx477,gpu_mem=256' | sudo tee -a /mnt/config.txt"
	@echo "   sudo umount /mnt"

ssh:
	ssh $(PI_REMOTE)

# Push source + scripts to the Pi (does not build remotely).
deploy:
	ssh $(PI_REMOTE) "mkdir -p $(PI_DIR)/bin"
	rsync -a --delete src CMakeLists.txt $(PI_REMOTE):$(PI_DIR)/
	rsync -a scripts/ $(PI_REMOTE):$(PI_DIR)/bin/

# Build on the Pi after deploy.
remote-build:
	ssh $(PI_REMOTE) "mkdir -p $(PI_DIR)/build && cd $(PI_DIR)/build && cmake .. && make -j\$$(nproc)"

# Clean the remote build dir.
remote-clean:
	ssh $(PI_REMOTE) "rm -rf $(PI_DIR)/build"

# Deploy + remote-build in one shot.
remote-deploy: deploy remote-build

# Run the binary on the Pi with given args, e.g.:
#   make remote-run ARGS="--list-controls"
remote-run:
	ssh $(PI_REMOTE) "$(PI_DIR)/build/picamera $(ARGS)"
