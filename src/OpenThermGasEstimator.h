/*
OpenThermGasEstimator.h - Estimates the gas that a boiler burns from its OpenTherm flame status and modulation level
Licensed under MIT license
*/

#ifndef OpenThermGasEstimator_h
#define OpenThermGasEstimator_h

#include <stdint.h>

class OpenThermGasEstimator
{
public:
    // A sample older than this does not count, because the flame and the modulation
    // may have changed any number of times since then.
    static const unsigned long maxSampleAge = 60000;

    // maxPower is the rated heat input of the boiler in kW, from its data plate.
    // minModulation is the power at a modulation level of 0 %, as a percentage of maxPower.
    // kWhPerCubicMeter is the calorific value of the gas, from the gas bill.
    OpenThermGasEstimator(float maxPower, float minModulation = 0, float kWhPerCubicMeter = 10.55f);

    // now is millis(). modulation is the relative modulation level in %.
    void update(unsigned long now, bool flameOn, float modulation);

    float getPower() const;  // kW, from the last sample
    float getEnergy() const; // kWh since the object was made
    float getGas() const;    // m³ since the object was made

private:
    const float maxPower;
    const float minModulation;
    const float kWhPerCubicMeter;

    bool hasSample;
    unsigned long sampleTime;
    uint32_t power; // W
    uint64_t energy; // W·ms, an integer so that small steps add up without rounding
};

#endif // OpenThermGasEstimator_h
