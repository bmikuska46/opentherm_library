/*
OpenThermGasEstimator.cpp - Estimates the gas that a boiler burns from its OpenTherm flame status and modulation level
Licensed under MIT license
*/

#include "OpenThermGasEstimator.h"

OpenThermGasEstimator::OpenThermGasEstimator(float maxPower, float minModulation, float kWhPerCubicMeter) :
    maxPower(maxPower),
    minModulation(minModulation),
    kWhPerCubicMeter(kWhPerCubicMeter),
    hasSample(false),
    sampleTime(0),
    power(0),
    energy(0)
{
}

void OpenThermGasEstimator::update(unsigned long now, bool flameOn, float modulation)
{
    const uint32_t age = (uint32_t)(now - sampleTime); // millis() wraps at 32 bits even where unsigned long is wider
    if (hasSample && age <= maxSampleAge)
        energy += (uint64_t)power * age;

    // The specification defines DATA-ID 17 as 0 % at the minimum modulation level and 100 % at the maximum.
    if (!(modulation > 0)) // also catches NaN
        modulation = 0;
    if (modulation > 100)
        modulation = 100;
    const float percent = minModulation + (100 - minModulation) * modulation / 100;
    power = flameOn ? (uint32_t)(maxPower * percent * 10 + 0.5f) : 0;

    hasSample = true;
    sampleTime = now;
}

float OpenThermGasEstimator::getPower() const
{
    return power / 1000.0f;
}

float OpenThermGasEstimator::getEnergy() const
{
    return energy / 3.6e9f;
}

float OpenThermGasEstimator::getGas() const
{
    return getEnergy() / kWhPerCubicMeter;
}
