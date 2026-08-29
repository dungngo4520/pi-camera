PI_HOST ?= raspberrypi.local
PI_USER ?= pi
PI_REMOTE := $(PI_USER)@$(PI_HOST)
PI_DIR    := ~/camera

.PHONY: all build clean flash ssh deploy remote-build remote-run remote-clean cross-build cross-deploy test test-sanitize tidy asan

all: build

build:
	mkdir -p build && cd build && cmake -DCMAKE_EXPORT_COMPILE_COMMANDS=ON .. && make -j$$(nproc)

clean:
	rm -rf build build-san

# Build + run unit tests (no sanitizers).
test: build
	cd build && ctest --output-on-failure

# Build + run unit tests under ASan + UBSan with leak detection.
test-sanitize: asan
	cd build-san && ASAN_OPTIONS=detect_leaks=1 ./picamera_tests

# Debug build with sanitizers enabled (PICAMERA_ENABLE_SANITIZERS=ON).
asan:
	mkdir -p build-san && cd build-san && cmake -DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DPICAMERA_ENABLE_SANITIZERS=ON .. && make -j$$(nproc) picamera_tests

# Run clang-tidy on all src/ files. Mirrors the CI filter: only fail on
# warnings originating in our src/ files (clang-tidy emits absolute paths
# from compile_commands.json, so match on the trailing /src/...cpp component).
tidy: build
	@clang-tidy -p build src/*.cpp 2>&1 | tee tidy.log
	@if grep -E "/src/[^/]+\.cpp:[0-9]+:[0-9]+: warning:" tidy.log; then \
		echo "clang-tidy found warnings in src/"; rm -f tidy.log; exit 1; \
	fi
	@rm -f tidy.log

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

# Cross-build picamera for aarch64 in Docker on this x86 host (no Pi needed).
# Output: ./picamera-arm64
cross-build:
	./scripts/cross-build.sh

# Cross-build locally, then scp the binary to the Pi.
cross-deploy: cross-build
	scp picamera-arm64 $(PI_REMOTE):$(PI_DIR)/build/picamera
	@echo "Deployed to $(PI_REMOTE):$(PI_DIR)/build/picamera"
