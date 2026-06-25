.PHONY: all build clean flash ssh deploy

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
	@echo "4. Mount and configure:"
	@echo "   sudo mount /dev/sdX1 /mnt"
	@echo "   sudo touch /mnt/ssh"
	@echo "   sudo cp config/wpa_supplicant.conf /mnt/"
	@echo "   echo 'dtoverlay=imx477,gpu_mem=256' | sudo tee -a /mnt/config.txt"
	@echo "   sudo umount /mnt"

ssh:
	ssh pi@picam.local

deploy: build
	ssh pi@picam.local "mkdir -p ~/camera/bin"
	scp -r src CMakeLists.txt pi@picam.local:~/camera/
	scp scripts/* pi@picam.local:~/camera/bin/
