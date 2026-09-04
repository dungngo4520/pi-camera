PI_HOST ?= raspberrypi.local
PI_USER ?= pi
PI_PASS  ?=
PI_REMOTE := $(PI_USER)@$(PI_HOST)
PI_DIR    := ~/camera

# SSH wrapper: uses sshpass if PI_PASS is set, otherwise plain ssh/rsync.
SSHPASS := $(if $(PI_PASS),SSHPASS='$(PI_PASS)' sshpass -e,)
SSH := $(SSHPASS) ssh
RSYNC := $(SSHPASS) rsync
SUDO := echo $(PI_PASS) | sudo -S

.PHONY: all build clean flash ssh deploy remote-build remote-run remote-clean cross-build cross-deploy test test-sanitize tidy asan install-service hw-test hw-deploy hw-restart hw-logs hw-status

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

# Run clang-tidy on src/ files that are in the build (compile_commands.json).
# Only processes files actually compiled — skips buttons.cpp/display.cpp when
# libgpiod is not installed (x86 dev builds). Mirrors the CI filter: only fail
# on warnings originating in our src/ files.
tidy: build
	@files=$$(python3 -c "import json;print(' '.join(e['file'] for e in json.load(open('build/compile_commands.json')) if '/src/' in e['file']))"); \
	clang-tidy -p build $$files > tidy.log 2>&1 || { cat tidy.log; rm -f tidy.log; exit 1; }
	@cat tidy.log
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
# Use -j1: Pi Zero 2 W has only 512MB RAM — parallel builds OOM (AGENTS.md).
remote-build:
	ssh $(PI_REMOTE) "mkdir -p $(PI_DIR)/build && cd $(PI_DIR)/build && cmake .. && make -j1"

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

# --- Hardware verification (requires Pi connected via SSH) ---
# These targets deploy, build, and test on a real Pi Zero 2 W + IMX477.
# Set PI_HOST/PI_USER/PI_PASS if your Pi isn't at raspberrypi.local.
# SSH auth: uses sshpass if PI_PASS is set, otherwise expects key-based auth.

# Deploy source + build on the Pi (does NOT restart the service).
hw-deploy:
	$(RSYNC) -a --delete src CMakeLists.txt config tests $(PI_REMOTE):$(PI_DIR)/
	$(SSH) $(PI_REMOTE) "cd $(PI_DIR) && mkdir -p build && cd build && cmake -DCMAKE_BUILD_TYPE=Release -DPICAMERA_BUILD_TESTS=ON .. && make -j1"
	@echo "=== Deployed + built on $(PI_HOST) ==="

# Full hardware test: deploy, build, run unit tests, test capture, verify service.
# Stops the picamera service first to free the camera for standalone capture tests,
# then restarts it at the end.
hw-test: hw-deploy
	@echo "=== STOP SERVICE (free camera) ==="
	-$(SSH) $(PI_REMOTE) "$(SUDO) systemctl stop picamera 2>/dev/null"
	@echo "=== UNIT TESTS ==="
	$(SSH) $(PI_REMOTE) "cd $(PI_DIR)/build && ctest --output-on-failure"
	@echo "=== CAPTURE TEST (JPEG) ==="
	$(SSH) $(PI_REMOTE) "cd $(PI_DIR) && ./build/picamera --capture hw_test.jpg --capture-dir /tmp --format jpeg 2>&1 && test -f /tmp/hw_test.jpg && echo 'JPEG capture: OK' && ls -la /tmp/hw_test.jpg || { echo 'JPEG capture: FAIL'; exit 1; }"
	@echo "=== CAPTURE TEST (DNG) ==="
	$(SSH) $(PI_REMOTE) "cd $(PI_DIR) && ./build/picamera --capture hw_test.dng --capture-dir /tmp --format dng 2>&1 && test -f /tmp/hw_test.dng && echo 'DNG capture: OK' && ls -la /tmp/hw_test.dng || { echo 'DNG capture: FAIL'; exit 1; }"
	@echo "=== RESTART SERVICE ==="
	$(SSH) $(PI_REMOTE) "$(SUDO) systemctl start picamera && sleep 3 && systemctl is-active picamera && echo 'Service: running'"
	@echo "=== ALL HW TESTS PASSED ==="

# Restart the picamera service on the Pi (appliance mode).
hw-restart:
	$(SSH) $(PI_REMOTE) "$(SUDO) systemctl restart picamera && sleep 3 && systemctl is-active picamera"
	@echo "=== picamera service restarted on $(PI_HOST) ==="

# Show picamera service status on the Pi.
hw-status:
	$(SSH) $(PI_REMOTE) "$(SUDO) systemctl status picamera 2>&1 | head -15"

# Show recent picamera journal logs on the Pi.
hw-logs:
	$(SSH) $(PI_REMOTE) "$(SUDO) journalctl -u picamera --no-pager -n 30 2>&1"

# Install picamera as a systemd service on the Pi (appliance mode).
# Run `make build` first as your normal user, then `sudo make install-service`.
# We deliberately do NOT depend on `build` here: that would run cmake+make as
# root and leave build/ owned by root, breaking subsequent non-root builds.
install-service:
	@test -f build/picamera || { echo "build/picamera not found — run 'make build' first (as non-root)"; exit 1; }
	install -m 0755 build/picamera /usr/local/bin/picamera
	install -m 0644 config/systemd/picamera.service /lib/systemd/system/picamera.service
	mkdir -p /home/pi/captures
	chown pi:pi /home/pi/captures
	chmod 700 /home/pi/captures
	systemctl daemon-reload
	systemctl enable picamera
	@echo "picamera service installed and enabled. Start with: sudo systemctl start picamera"
