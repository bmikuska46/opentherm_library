/*
OpenTherm Gas Estimate Example

Keeps central heating and hot water on, and estimates the power, energy, and gas that the boiler burns
from its flame status and relative modulation level. Prints the estimate every 10 s at 9600 baud.

Set maxPower, minModulation, and kWhPerCubicMeter for your boiler and your gas supply.
At startup the sketch prints the capacity and minimum modulation level that the boiler reports, if it reports them.
*/

#include <Arduino.h>
#include <OpenTherm.h>
#include <OpenThermGasEstimator.h>

const int inPin = 2;  // for Arduino, 4 for ESP8266 (D2), 21 for ESP32
const int outPin = 3; // for Arduino, 5 for ESP8266 (D1), 22 for ESP32
OpenTherm ot(inPin, outPin);

const float maxPower = 24;            // kW, the rated heat input on the boiler's data plate
const float minModulation = 20;       // %, the lowest power as a percentage of maxPower
const float kWhPerCubicMeter = 10.55; // the calorific value on your gas bill
OpenThermGasEstimator gas(maxPower, minModulation, kWhPerCubicMeter);

unsigned long lastPrint = 0;

void IRAM_ATTR handleInterrupt()
{
    ot.handleInterrupt();
}

void setup()
{
    Serial.begin(9600);
    if (!ot.begin(handleInterrupt))
        Serial.println("Error: the input pin does not support interrupts");

    unsigned char maxCapacity;
    unsigned char minModulationLevel;
    if (ot.getMaxCapacity(maxCapacity, minModulationLevel))
        Serial.println("The boiler reports " + String(maxCapacity) + " kW at most and a minimum modulation level of " + String(minModulationLevel) + " %");
}

void loop()
{
    unsigned long response = ot.setBoilerStatus(true, true);
    float modulation;
    if (OpenTherm::isValidResponse(response) && ot.getModulation(modulation))
        gas.update(millis(), OpenTherm::isFlameOn(response), modulation);

    ot.setCHSetpoint(64);

    if (millis() - lastPrint >= 10000)
    {
        lastPrint = millis();
        Serial.println("Power " + String(gas.getPower(), 1) + " kW, energy " + String(gas.getEnergy(), 3) + " kWh, gas " + String(gas.getGas(), 3) + " m3");
    }
}
