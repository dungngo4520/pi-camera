#include "battery.h"

#include <iostream>
#include <cstring>
#include <cmath>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/i2c-dev.h>

namespace picamera {

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
    cfg |= (uint16_t)((0x4 | (channel & 0x3)) << 12); // MUX: single-ended
    cfg |= (pgaGain & 0x7) << 9;  // PGA
    cfg |= (0 << 8);              // MODE: continuous
    cfg |= (0x4 << 5);            // DR: 128 SPS
    cfg |= 0x0003;                // CQUE: disable comparator
    return cfg;
}

} // namespace

bool BatteryMonitor::init(const BatteryConfig &cfg) {
    cfg_ = cfg;

    // Determine LSB size from PGA gain
    switch (cfg_.pgaGain) {
        case 0x0000: lsbMillivolts_ = 0.1875; break; // ±6.144V
        case 0x0001: lsbMillivolts_ = 0.125;   break; // ±4.096V
        case 0x0002: lsbMillivolts_ = 0.0625;  break; // ±2.048V
        case 0x0003: lsbMillivolts_ = 0.03125; break; // ±1.024V
        default:     lsbMillivolts_ = 0.1875;  break; // default ±6.144V
    }

    fd_ = open(cfg_.i2cDevice.c_str(), O_RDWR);
    if (fd_ < 0) {
        std::cerr << "Battery: cannot open " << cfg_.i2cDevice
                  << ": " << strerror(errno) << "\n";
        return false;
    }

    if (ioctl(fd_, I2C_SLAVE, cfg_.i2cAddress) < 0) {
        std::cerr << "Battery: cannot set I2C slave 0x"
                  << std::hex << static_cast<int>(cfg_.i2cAddress)
                  << ": " << strerror(errno) << "\n";
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

    std::cout << "Battery: ADS1115 at 0x" << std::hex
              << static_cast<int>(cfg_.i2cAddress) << std::dec
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
    if (::write(fd_, buf, 3) != 3) {
        std::cerr << "Battery: config write failed: " << strerror(errno) << "\n";
        return false;
    }
    return true;
}

bool BatteryMonitor::readConversion(uint16_t &raw) const {
    // Point to conversion register
    uint8_t reg = ADS_CONV;
    if (::write(fd_, &reg, 1) != 1) {
        return false;
    }
    // Read 2 bytes (MSB first)
    uint8_t data[2];
    if (::read(fd_, data, 2) != 2) {
        return false;
    }
    raw = (static_cast<uint16_t>(data[0]) << 8) | data[1];
    return true;
}

double BatteryMonitor::rawToVoltage(uint16_t raw) const {
    // ADS1115 is 16-bit signed. Single-ended reads are positive.
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

    result.voltage = rawToVoltage(raw);
    result.percent = lipoVoltageToPercent(result.voltage);
    result.valid = true;
    return result;
}

} // namespace picamera
