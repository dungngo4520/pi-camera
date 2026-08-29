#pragma once

#include <cstdint>
#include <string>

namespace picamera {

struct BatteryConfig {
    std::string i2cDevice = "/dev/i2c-1";
    uint8_t i2cAddress = 0x48;       // ADS1115 default (ADDR -> GND)
    uint8_t channel = 0;             // A0 (single-ended)
    uint16_t pgaGain = 0x0001;       // PGA config: ±4.096V full scale
    // With ±4.096V range and 16-bit, LSB = 125µV.
    // LiPo max 4.2V slightly exceeds 4.096V, so we use ±6.144V (gain 0x0002,
    // LSB=187.5µV) for headroom. Set pgaGain=0x0002 for direct battery read.
    // Alternatively use ±4.096V with a voltage divider for better resolution.
};

struct BatteryReading {
    bool valid = false;
    double voltage = 0.0;   // volts
    int percent = 0;        // 0-100 (LiPo discharge curve estimate)
};

// ADS1115 I2C ADC battery monitor.
// Reads single-ended voltage from the ADS1115 and estimates LiPo state
// of charge from the discharge curve. Uses raw i2c-dev ioctl — no
// external library dependency beyond the kernel i2c-dev module.
class BatteryMonitor {
public:
    bool init(const BatteryConfig &cfg = {});
    void shutdown();

    // Read current battery voltage and estimate SOC.
    // Returns valid=false if the I2C read fails.
    BatteryReading read();

private:
    bool writeConfig(uint16_t config) const;
    bool readConversion(uint16_t &raw) const;
    double rawToVoltage(uint16_t raw) const;

    BatteryConfig cfg_;
    int fd_ = -1;
    double lsbMillivolts_ = 0.1875; // depends on PGA gain
};

// Convert LiPo cell voltage to estimated state-of-charge (0-100%).
// Uses a piecewise-linear approximation of the discharge curve.
// Input: voltage in volts (typically 3.0 - 4.2).
int lipoVoltageToPercent(double voltage);

} // namespace picamera
