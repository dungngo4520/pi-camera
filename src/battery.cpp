#include "battery.h"
#include "safe_path.h"

#include <algorithm>
#include <iostream>
#include <cstring>
#include <cmath>
#include <cstdio>
#include <chrono>
#include <thread>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <cerrno>

namespace picamera {

namespace {
// Retry short writes on I2C up to 3 times. Returns total bytes written, or -1.
ssize_t i2cWrite(int fd, const void *buf, size_t len) {
    size_t done = 0;
    int errors = 0;
    while (done < len && errors < 3) {
        ssize_t n = ::write(fd, static_cast<const char *>(buf) + done, len - done);
        if (n < 0) {
            if (errno == EINTR) { continue; }  // retry without counting
            ++errors;
            continue;
        }
        if (n == 0) { ++errors; continue; }
        done += static_cast<size_t>(n);
        // Reset error count on successful progress.
        errors = 0;
    }
    if (done != len) { errno = EIO; return -1; }
    return static_cast<ssize_t>(done);
}
} // namespace

// --- LiPo discharge curve (voltage -> SOC%) ---
// Piecewise-linear approximation of a typical 1000mAh LiPo discharge.
// Points are (voltage, percent). Voltage is open-circuit rest voltage;
// under load the voltage sags, so readings are approximate (±5-10%).
struct VoltagePoint { double v; int pct; };
static const VoltagePoint kLipoCurve[] = {
    {4.20, 100},
    {4.10,  90},
    {4.00,  80},
    {3.90,  70},
    {3.80,  55},
    {3.70,  40},
    {3.60,  25},
    {3.50,  15},
    {3.40,   8},
    {3.30,   5},
    {3.00,   0},
};
static const int kLipoCurveLen = sizeof(kLipoCurve) / sizeof(kLipoCurve[0]);

int lipoVoltageToPercent(double voltage) {
    // NaN/Inf from a corrupt ADC read should not silently report 0%.
    if (!std::isfinite(voltage)) return -1;
    // Clamp to curve range
    if (voltage >= kLipoCurve[0].v) return 100;
    if (voltage <= kLipoCurve[kLipoCurveLen - 1].v) return 0;

    // Find the segment containing this voltage and interpolate
    for (int i = 0; i < kLipoCurveLen - 1; ++i) {
        const auto &hi = kLipoCurve[i];
        const auto &lo = kLipoCurve[i + 1];
        if (voltage <= hi.v && voltage >= lo.v) {
            double t = (voltage - lo.v) / (hi.v - lo.v);
            return static_cast<int>(std::round(lo.pct + t * (hi.pct - lo.pct)));
        }
    }
    return 0; // unreachable
}

// --- ADS1115 register addresses ---
enum Ads1115Reg : uint8_t {
    ADS_CONV  = 0x00, // Conversion register
    ADS_CFG   = 0x01, // Config register
    ADS_LOTHR = 0x02, // Lo threshold
    ADS_HITHR = 0x03, // Hi threshold
};

// ADS1115 config register bits:
// [15]    OS=1       Start single conversion
// [14:12] MUX        Input multiplexer (0x4 = AIN0 single-ended)
// [11:9]  PGA        Gain amplifier
// [8]     MODE=0     Continuous conversion
// [7:5]   DR=0x4     128 SPS (good balance of speed/noise)
// [4]     CMODE=0    Traditional comparator
// [3]     CPOL=0     Active low
// [2]     CLAT=0     Non-latching
// [1:0]   CQUE=0x3   Disable comparator
namespace {

uint16_t buildConfig(uint8_t channel, uint16_t pgaGain) {
    uint16_t cfg = 0;
    cfg |= (1 << 15);              // OS: start conversion
    cfg |= static_cast<uint16_t>((0x4 | (channel & 0x3)) << 12); // MUX: single-ended
    cfg |= (pgaGain & 0x7) << 9;  // PGA
    cfg |= (0 << 8);              // MODE: continuous
    cfg |= (0x4 << 5);            // DR: 128 SPS
    cfg |= 0x0003;                // CQUE: disable comparator
    return cfg;
}

} // namespace

bool BatteryMonitor::init(const BatteryConfig &cfg) {
    cfg_ = cfg;

    // Validate the device path (defense-in-depth — parseArgs also checks).
    if (!isSafeDevicePath(cfg_.i2cDevice)) {
        std::cerr << "Battery: unsafe device path: " << cfg_.i2cDevice << "\n";
        return false;
    }

    // Determine LSB size from PGA gain.
    // ADS1115 only supports PGA codes 0-3; values 4-7 are reserved.
    switch (cfg_.pgaGain) {
        case 0x0000: lsbMillivolts_ = 0.1875; break; // ±6.144V
        case 0x0001: lsbMillivolts_ = 0.125;   break; // ±4.096V
        case 0x0002: lsbMillivolts_ = 0.0625;  break; // ±2.048V
        case 0x0003: lsbMillivolts_ = 0.03125; break; // ±1.024V
        default:
            std::cerr << "Battery: invalid PGA gain 0x" << std::hex
                      << cfg_.pgaGain << std::dec
                      << " (only 0-3 supported)\n";
            return false;
    }

    // O_NOFOLLOW is not needed for I2C character devices — isSafeDevicePath
    // already validates the path, and O_NOFOLLOW could reject legitimate
    // symlinked device names (e.g. /dev/i2c-1 -> /dev/i2c/1-0048).
    fd_ = open(cfg_.i2cDevice.c_str(), O_RDWR | O_CLOEXEC);
    if (fd_ < 0) {
        std::cerr << "Battery: cannot open " << cfg_.i2cDevice
                  << ": " << errnoString(errno) << "\n";
        return false;
    }

    // Validate 7-bit I2C address range (0x03–0x77 per I2C spec).
    // Addresses 0x00–0x02 and 0x78–0x7F are reserved.
    if (cfg_.i2cAddress < 0x03 || cfg_.i2cAddress > 0x77) {
        std::cerr << "Battery: invalid I2C address 0x"
                  << std::hex << static_cast<unsigned>(cfg_.i2cAddress)
                  << std::dec << " (must be 0x03–0x77)\n";
        close(fd_);
        fd_ = -1;
        return false;
    }

    if (ioctl(fd_, I2C_SLAVE, cfg_.i2cAddress) < 0) {
        char addrBuf[8];
        std::snprintf(addrBuf, sizeof(addrBuf), "%02X",
                      static_cast<unsigned>(cfg_.i2cAddress));
        std::cerr << "Battery: cannot set I2C slave 0x" << addrBuf
                  << ": " << errnoString(errno) << "\n";
        close(fd_);
        fd_ = -1;
        return false;
    }

    // Write initial config to start continuous conversions
    if (!writeConfig(buildConfig(cfg_.channel, cfg_.pgaGain))) {
        close(fd_);
        fd_ = -1;
        return false;
    }

    // Wait for the first conversion to complete before returning.
    // The config write sets the OS bit to start a new conversion, but at
    // 128 SPS the first result takes ~8ms. Without this delay, the first
    // read() could return stale or invalid data from before the config change.
    std::this_thread::sleep_for(std::chrono::milliseconds(10)); // 10ms > 1/128s ≈ 7.8ms

    char addrBuf[8];
    std::snprintf(addrBuf, sizeof(addrBuf), "%02X",
                  static_cast<unsigned>(cfg_.i2cAddress));
    std::cout << "Battery: ADS1115 at 0x" << addrBuf
              << " initialized (channel " << static_cast<int>(cfg_.channel)
              << ", LSB=" << lsbMillivolts_ << "mV)\n";
    return true;
}

void BatteryMonitor::shutdown() {
    if (fd_ >= 0) {
        close(fd_);
        fd_ = -1;
    }
}

bool BatteryMonitor::writeConfig(uint16_t config) const {
    uint8_t buf[3];
    buf[0] = ADS_CFG;
    buf[1] = static_cast<uint8_t>(config >> 8);   // MSB first
    buf[2] = static_cast<uint8_t>(config & 0xFF);
    if (i2cWrite(fd_, buf, 3) < 0) {
        std::cerr << "Battery: config write failed: " << errnoString(errno) << "\n";
        return false;
    }
    return true;
}

bool BatteryMonitor::readConversion(uint16_t &raw) const {
    // Use I2C_RDWR to perform a combined repeated-START transaction:
    // write the pointer register, then read 2 data bytes in one ioctl.
    // Separate ::write/::read calls may insert a STOP between transactions,
    // causing the ADS1115 to lose the pointer and return wrong data.
    uint8_t reg = ADS_CONV;
    uint8_t data[2] = {0, 0};
    i2c_msg msgs[2];
    msgs[0].addr = cfg_.i2cAddress;
    msgs[0].flags = 0;  // write
    msgs[0].len = 1;
    msgs[0].buf = &reg;
    msgs[1].addr = cfg_.i2cAddress;
    msgs[1].flags = I2C_M_RD;  // read
    msgs[1].len = 2;
    msgs[1].buf = data;
    i2c_rdwr_ioctl_data ioctl_data;
    ioctl_data.msgs = msgs;
    ioctl_data.nmsgs = 2;
    // I2C_RDWR returns the number of messages successfully transferred
    // (or -1 on error). It does NOT write back into ioctl_data.nmsgs,
    // so we must check the return value, not the struct field.
    int rc = ioctl(fd_, I2C_RDWR, &ioctl_data);
    if (rc < 0) {
        std::cerr << "Battery: I2C_RDWR failed: " << errnoString(errno) << "\n";
        return false;
    }
    if (rc != 2) {
        std::cerr << "Battery: I2C_RDWR partial transfer (" << rc << "/2)\n";
        return false;
    }
    raw = (static_cast<uint16_t>(data[0]) << 8) | data[1];
    return true;
}

double BatteryMonitor::rawToVoltage(uint16_t raw) const {
    // ADS1115 is 16-bit signed. Single-ended reads are positive (0–0x7FFF).
    // read() rejects out-of-range values before calling this.
    int16_t signed_raw = static_cast<int16_t>(raw);
    double millivolts = static_cast<double>(signed_raw) * lsbMillivolts_;
    return millivolts / 1000.0;
}

BatteryReading BatteryMonitor::read() {
    BatteryReading result;
    if (fd_ < 0) return result;

    uint16_t raw;
    if (!readConversion(raw)) {
        return result;
    }

    // ADS1115 is 16-bit signed. In single-ended mode, valid readings are
    // 0–0x7FFF. Values above 0x7FFF indicate a corrupt/noisy read (or
    // negative voltage), not a full battery — treat as invalid.
    if (raw > 0x7FFF) {
        std::cerr << "Battery: out-of-range ADC reading (0x"
                  << std::hex << raw << std::dec << ")\n";
        return result;
    }

    result.voltage = rawToVoltage(raw);
    result.percent = lipoVoltageToPercent(result.voltage);
    result.valid = (result.percent >= 0);
    return result;
}

} // namespace picamera
