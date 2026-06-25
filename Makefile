.PHONY: all build clean flash ssh deploy

all: build

# Build the picamera binary using CMake.
build:
	mkdir -p build && cd build && cmake .. && make -j$$(nproc)

clean:
	rm -rf build

# Print instructions for flashing the Pi OS to a microSD card via rpiboot.
# The Pi Zero 2 must be connected via USB and show as "BCM2710 Boot".
flash:
	@echo ""
	@echo "=== Flash Pi OS via USB (rpiboot) ==="
	@echo ""
	@echo "1. Insert microSD card into Pi Zero 2"
	@echo "2. Ensure Pi is connected via USB (shows as BCM2710 Boot)"
	@echo "3. Run: sudo rpiboot"
	@echo "4. Pi will reboot and appear as a USB mass storage device"
	@echo "5. Find the device: lsblk"
	@echo "6. Flash the OS:"
	@echo "   xzcat raspios-lite-arm64.img.xz | sudo dd of=/dev/sdX bs=4M status=progress"
	@echo "7. Mount and configure:"
	@echo "   sudo mount /dev/sdX1 /mnt"
	@echo "   sudo touch /mnt/ssh"
	@echo "   sudo cp config/wpa_supplicant.conf /mnt/"
	@echo "   echo 'dtoverlay=imx477,gpu_mem=256' | sudo tee -a /mnt/config.txt"
	@echo "   sudo umount /mnt"
	@echo ""

ssh:
	ssh pi@picam.local

# Deploy source code and build system files to the Pi via scp.
# The Pi must be reachable at picam.local with SSH key or password auth.
deploy: build
	@echo "Deploying source to pi@picam.local:~/camera/ ..."
	scp -r src CMakeLists.txt pi@picam.local:~/camera/
	@echo ""
	@echo "On the Pi, build with:"
	@echo "  cd ~/camera && mkdir -p build && cd build && cmake .. && make -j4"
	@echo ""
