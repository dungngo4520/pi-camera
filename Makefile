PI_HOST ?= raspberrypi.local
PI_USER ?= pi
PI_PASS  ?=
PI_REMOTE := $(PI_USER)@$(PI_HOST)
PI_DIR    := ~/camera

SSHPASS := $(if $(PI_PASS),SSHPASS='$(PI_PASS)' sshpass -e,)
SSH := $(SSHPASS) ssh
RSYNC := $(SSHPASS) rsync
SUDO := echo $(PI_PASS) | sudo -S

.PHONY: all build clean test test-sanitize tidy deploy remote-deploy remote-run remote-clean cross-build cross-deploy flash hw-deploy hw-test hw-restart hw-status hw-logs install-service

all: build

build:
	mkdir -p build && cd build && cmake -DCMAKE_EXPORT_COMPILE_COMMANDS=ON .. && make -j$$(nproc)

clean:
	rm -rf build build-san

test: build
	cd build && ctest --output-on-failure

test-sanitize:
	mkdir -p build-san && cd build-san && cmake -DCMAKE_BUILD_TYPE=Debug -DPICAMERA_ENABLE_SANITIZERS=ON .. && make -j$$(nproc) picamera_tests
	cd build-san && ASAN_OPTIONS=detect_leaks=1 ./picamera_tests

tidy: build
	@files=$$(python3 -c "import json;print(' '.join(e['file'] for e in json.load(open('build/compile_commands.json')) if '/src/' in e['file']))"); \
	clang-tidy -p build $$files > tidy.log 2>&1 || { cat tidy.log; rm -f tidy.log; exit 1; }
	@if grep -E "/src/[^/]+\.cpp:[0-9]+:[0-9]+: warning:" tidy.log; then exit 1; fi
	@rm -f tidy.log

deploy:
	$(RSYNC) -a --delete src CMakeLists.txt config tests $(PI_REMOTE):$(PI_DIR)/
	$(SSH) $(PI_REMOTE) "cd $(PI_DIR) && mkdir -p build && cd build && cmake -DCMAKE_BUILD_TYPE=Release -DPICAMERA_BUILD_TESTS=ON .. && make -j1"

remote-deploy: deploy
remote-run:
	$(SSH) $(PI_REMOTE) "$(PI_DIR)/build/picamera $(ARGS)"
remote-clean:
	$(SSH) $(PI_REMOTE) "rm -rf $(PI_DIR)/build"

cross-build:
	./scripts/cross-build.sh
cross-deploy: cross-build
	scp picamera-arm64 $(PI_REMOTE):$(PI_DIR)/build/picamera

flash:
	@echo "Flash Pi OS: insert microSD, connect Pi via USB, run sudo rpiboot,"
	@echo "then xzcat raspios-lite-arm64.img.xz | sudo dd of=/dev/sdX bs=4M"
	@echo "Mount boot partition, enable SSH, add wpa_supplicant.conf,"
	@echo "add 'dtoverlay=imx477,gpu_mem=256' to config.txt"

hw-test: deploy
	-$(SSH) $(PI_REMOTE) "$(SUDO) systemctl stop picamera 2>/dev/null"
	$(SSH) $(PI_REMOTE) "cd $(PI_DIR)/build && ctest --output-on-failure"
	$(SSH) $(PI_REMOTE) "cd $(PI_DIR) && ./build/picamera --capture hw_test.jpg --capture-dir /tmp --format jpeg 2>&1 && test -f /tmp/hw_test.jpg && echo 'JPEG: OK' || { echo 'JPEG: FAIL'; exit 1; }"
	$(SSH) $(PI_REMOTE) "cd $(PI_DIR) && ./build/picamera --capture hw_test.dng --capture-dir /tmp --format dng 2>&1 && test -f /tmp/hw_test.dng && echo 'DNG: OK' || { echo 'DNG: FAIL'; exit 1; }"
	$(SSH) $(PI_REMOTE) "$(SUDO) systemctl start picamera && sleep 3 && systemctl is-active picamera"

hw-deploy: deploy
	$(SSH) $(PI_REMOTE) "$(SUDO) install -m 0644 $(PI_DIR)/config/systemd/picamera.service /usr/lib/systemd/system/picamera.service && $(SUDO) systemctl daemon-reload && $(SUDO) systemctl restart picamera && sleep 3 && systemctl is-active picamera"
hw-restart:
	$(SSH) $(PI_REMOTE) "$(SUDO) systemctl restart picamera && sleep 3 && systemctl is-active picamera"
hw-status:
	$(SSH) $(PI_REMOTE) "$(SUDO) systemctl status picamera 2>&1 | head -15"
hw-logs:
	$(SSH) $(PI_REMOTE) "$(SUDO) journalctl -u picamera --no-pager -n 30 2>&1"

install-service:
	install -m 0644 config/systemd/picamera.service /lib/systemd/system/picamera.service
	mkdir -p /home/pi/captures /home/pi/.config/picamera && chown pi:pi /home/pi/captures /home/pi/.config/picamera && chmod 700 /home/pi/captures /home/pi/.config/picamera
	systemctl daemon-reload && systemctl enable picamera
