# OpenThermPlus

OpenThermPlus is an OpenTherm library for Arduino, ESP8266, and ESP32. It lets a microcontroller speak OpenTherm, the protocol that room thermostats use to control boilers. It follows the OpenTherm Protocol Specification v2.2, which this README calls the specification. You can use it to build:

- A master, such as a thermostat that reads and sets boiler values.
- A slave, such as a boiler simulator for testing a thermostat.
- A gateway that sits between a thermostat and a boiler and logs their messages.

## Connect the hardware

The OpenTherm line runs at voltages that a microcontroller pin cannot drive or read directly. Put an adapter between the boiler and the board, such as the [OpenTherm Adapter](https://ihormelnyk.com/opentherm_adapter). Its page has the [schematic](https://ihormelnyk.com/Content/Pages/opentherm_adapter/opentherm_adapter_schematic.png) and an [Arduino Uno wiring diagram](https://ihormelnyk.com/Content/Pages/opentherm_adapter/opentherm_adapter_arduino_connection.png).

The board needs two pins:

- An input pin that reads the adapter's OUT pin. It must support interrupts, because the library receives frames in a pin interrupt.
- An output pin that drives the adapter's IN pin.

The master example uses these pins:

| Board | Input pin | Output pin |
| --- | --- | --- |
| Arduino Uno, Nano, or Mini | 2 | 3 |
| ESP8266 | 4 (D2) | 5 (D1) |
| ESP32 | 21 | 22 |

On an Arduino Uno, Nano, or Mini, only pins 2 and 3 support interrupts. On a Mega, pins 2, 3, 18, 19, 20, and 21 do. On an ESP8266, every GPIO except GPIO16 does.

## Install the library

- **Arduino IDE.** Open **Sketch > Include Library > Manage Libraries**, search for `OpenThermPlus`, and click **Install**. With arduino-cli, run this command:
	```sh
	arduino-cli lib install OpenThermPlus
	```
	If the original OpenTherm Library by Ihor Melnyk is also installed, remove it. Both provide `OpenTherm.h`, and the IDE uses only one of them.
- **PlatformIO.** Add the library to `lib_deps` in `platformio.ini`.
	```ini
	lib_deps = bmikuska46/OpenThermPlus@^2.2.0
	```
- **ESP-IDF with the Arduino component.** Clone the repository into the `components` folder of your project. Its `CMakeLists.txt` registers it as a component that requires `arduino`.

## Talk to a boiler

This sketch turns on central heating and hot water, sets the CH water setpoint to 64 °C, and prints whether the flame is on.

```cpp
#include <Arduino.h>
#include <OpenTherm.h>

const int inPin = 2;
const int outPin = 3;
OpenTherm ot(inPin, outPin);

void IRAM_ATTR handleInterrupt()
{
	ot.handleInterrupt();
}

void setup()
{
	Serial.begin(9600);
	ot.begin(handleInterrupt);
}

void loop()
{
	unsigned long response = ot.setBoilerStatus(true, true);
	OpenThermResponseStatus status = ot.getLastResponseStatus();
	if (status == OpenThermResponseStatus::SUCCESS)
		Serial.println(ot.isFlameOn(response) ? "Flame on" : "Flame off");
	else
		Serial.println(OpenTherm::statusToString(status));

	ot.setCHSetpoint(64);
}
```

`IRAM_ATTR` keeps the handler in RAM on ESP8266 and ESP32. The library defines it as empty on other boards. On ESP8266 and ESP32, you can call `ot.begin()` with no handler, and the library attaches its own. `begin()` sets the line idle, waits 1 s, and returns `true`. It returns `false` at once if the board reports that the input pin has no interrupt, or on AVR boards if the handler is `NULL`.

Each call in `loop()` blocks until the conversation ends. With an answer, that is 100 ms after the answer, the pause that the specification requires between conversations. Without one, it takes about 1 s. The loop has no `delay()` on purpose. The specification requires the master to communicate at least once per second, with a 15% tolerance, and a `delay(1000)` between requests makes the gap longer than that.

These calls each send one request and wait for the answer:

| Method | Returns |
| --- | --- |
| `setBoilerStatus(enableCentralHeating, enableHotWater, ...)` | The boiler's status frame. Read it with `isFault()`, `isCentralHeatingActive()`, `isHotWaterActive()`, `isFlameOn()`, `isCoolingActive()`, `isCentralHeating2Active()`, and `isDiagnostic()`. |
| `setCHSetpoint(temperature)` | `true` if the boiler accepted the CH water setpoint. The original name, `setBoilerTemperature()`, also works. |
| `setDHWSetpoint(temperature)` | `true` if the boiler accepted the hot water setpoint. |
| `getBoilerTemperature()` | The CH flow water temperature in °C. |
| `getReturnTemperature()` | The return water temperature in °C. |
| `getDHWTemperature()` | The hot water temperature in °C. |
| `getModulation()` | The relative modulation level in %. |
| `getPressure()` | The CH water pressure in bar. |
| `getFault()` | The application-specific fault flags. |

The getters return 0 when the conversation fails. A modulation level, a pressure, or the fault flags can also be a real 0. To tell the two apart, pass a variable to the getter. It returns `true` and sets the variable if the conversation succeeds. Otherwise it returns `false` and leaves the variable unchanged.

```cpp
float modulation;
if (ot.getModulation(modulation))
	Serial.println(modulation);
```

`getFault(flags, oemCode)` works the same way and sets both bytes of the answer, the fault flags and the OEM fault code. `getMaxCapacity(maxCapacity, minModulation)` reads DATA-ID 15 the same way. It sets the maximum boiler capacity in kW and the minimum modulation level in %. Many boilers do not support DATA-ID 15.

For any other DATA-ID, build the frame yourself and read the value from the answer:

```cpp
unsigned long request = OpenTherm::buildRequest(OpenThermMessageType::READ_DATA, OpenThermMessageID::Toutside, 0);
unsigned long response = ot.sendRequest(request);
if (OpenTherm::isValidResponse(response))
	Serial.println(OpenTherm::getFloat(response));
```

`getUInt()` returns the 16-bit data value, and `getFloat()` decodes it as the specification's f8.8 format. `temperatureToData()` encodes a temperature for a `WRITE_DATA` request and limits it to the range 0 to 100 °C. `floatToData()` encodes any f8.8 value, including a negative one such as an outside temperature, and limits it to the range -128 to 127.996. It encodes NaN, which a failed sensor read often returns, as -128.

## Check the result of a conversation

`getLastResponseStatus()` returns the result of the last conversation:

| Status | Meaning |
| --- | --- |
| `SUCCESS` | The answer is a `READ_ACK` or `WRITE_ACK` with the DATA-ID of the request and correct parity. |
| `REJECTED` | The boiler answered `DATA_INVALID` or `UNKNOWN_DATA_ID` with the right DATA-ID and parity. It received the request and refused it. |
| `INVALID` | A frame arrived, but it has bad parity, a stop bit of 0, a missing mid-bit transition, a different DATA-ID, or a message type that only a master sends. |
| `TIMEOUT` | No complete frame arrived within 1 s of the end of the request. |
| `NONE` | No conversation has finished yet, or the current one is still running. |

On a master, a frame that would pass `isValidResponse()` reaches your code only with `SUCCESS`. With any other status, `sendRequest()`, the callback, and `getLastResponse()` after the conversation get 0 in its place. Other frames keep their bits for diagnostics, so do not compare the frame with 0. `OpenTherm::isValidResponse(response)` is `true` when the conversation that returned `response` succeeded, and `false` otherwise. While a conversation runs, `getLastResponse()` returns the bits received so far.

`sendRequest()` returns 0 and leaves the status as it was when it cannot start a conversation. That happens before `begin()`, after `end()`, and inside the callback, where the conversation that called the callback has not ended yet. `getLastResponseStatus()` then still returns the status of the previous conversation, so check the frame with `isValidResponse()` instead.

## Send requests without blocking

`sendRequestAsync()` sends the request, which takes about 34 ms, and returns without waiting for the answer. It returns `false` and sends nothing until the previous conversation ends. Call `process()` from `loop()`. When a conversation ends, `process()` calls the function that you passed to `begin()`, with the answer and its status.

```cpp
void processResponse(unsigned long response, OpenThermResponseStatus status)
{
	if (status == OpenThermResponseStatus::SUCCESS)
		Serial.println(OpenTherm::getFloat(response));
}

void setup()
{
	ot.begin(handleInterrupt, processResponse);
}

void loop()
{
	ot.process();
	ot.sendRequestAsync(OpenTherm::buildGetBoilerTemperatureRequest());
}
```

On ESP8266 and ESP32, `begin()` also accepts a `std::function`, such as a lambda that captures state.

## Estimate gas consumption

OpenTherm has no DATA-ID for gas flow, so the library cannot read how much gas the boiler burns. `OpenThermGasEstimator` estimates it from the flame status and the relative modulation level that the boiler reports. Pass it a sample after each successful read:

```cpp
#include <OpenThermGasEstimator.h>

OpenThermGasEstimator gas(24, 20, 10.55); // max power in kW, min modulation in %, kWh per m³

void loop()
{
	unsigned long response = ot.setBoilerStatus(true, true);
	float modulation;
	if (OpenTherm::isValidResponse(response) && ot.getModulation(modulation))
		gas.update(millis(), OpenTherm::isFlameOn(response), modulation);
}
```

`getPower()` returns the power of the last sample in kW. `getEnergy()` returns the energy in kWh, and `getGas()` returns the gas in m³, both since the object was made. To keep a total across restarts, save the total yourself and add `getEnergy()` to it.

The estimator needs three numbers:

- `maxPower` is the rated heat input of the boiler in kW, from its data plate. Use the heat input, not the heat output, because the gas burned is the input. `getMaxCapacity()` can give an approximate value, but the specification does not say whether it is the input or the output.
- `minModulation` is the lowest power of the burner, as a percentage of `maxPower`. The specification defines a modulation level of 0 % as the minimum modulation level and 100 % as the maximum, so with the flame on at 0 % the boiler still burns gas. `getMaxCapacity()` can give this value. Some boilers report the modulation level as a percentage of the maximum power instead. For those boilers, set `minModulation` to 0.
- `kWhPerCubicMeter` is the calorific value of the gas, from the gas bill. It defaults to 10.55 kWh/m³, a typical value for natural gas.

The estimator holds each sample until the next one. Between two samples, it counts the power of the first. It ignores a sample older than 1 minute, so a gap in the readings counts as no gas rather than as a guess. The more often you sample, the closer the estimate follows the burner. It adds energy as an integer, so long runs of small steps do not lose precision.

The result is an estimate. The power of a real burner does not follow its modulation level exactly, and the rated heat input is a nominal value. To improve the estimate, compare `getGas()` with your gas meter over a few days and scale `maxPower` by the ratio.

## Build a slave or a gateway

To act as a slave, pass `OpenThermRole::SLAVE` as the third argument of the constructor. A sketch written for the original library passes `true`, which also works. The library calls your function for each request, and the function answers with `sendResponse()`.

```cpp
OpenTherm ot(inPin, outPin, OpenThermRole::SLAVE);

void processRequest(unsigned long request, OpenThermResponseStatus status)
{
	if (status != OpenThermResponseStatus::SUCCESS)
		return;
	OpenThermMessageID id = OpenTherm::getDataID(request);
	ot.sendResponse(OpenTherm::buildResponse(OpenThermMessageType::UNKNOWN_DATA_ID, id, 0));
}

void setup()
{
	ot.begin(handleInterrupt, processRequest);
}

void loop()
{
	ot.process();
}
```

Return without an answer when the status is not `SUCCESS`. Section 4.5 of the specification says to reject a frame with errors and end the conversation. A request with `INVALID_DATA` is `SUCCESS`, because section 4.4.3 expects an answer to it. Call `sendResponse()` straight from the callback. It waits until 20 ms have passed since the end of the request and then sends.

The `examples` folder has four sketches:

- `OpenThermMaster_Demo` sets the boiler status, the CH setpoint, and the hot water setpoint, reads the boiler and hot water temperatures, and prints the results.
- `OpenThermSlave_Demo` answers the status, CH setpoint, and boiler temperature DATA-IDs, and answers `UNKNOWN_DATA_ID` to the rest. It answers an `INVALID_DATA` request with `DATA_INVALID`.
- `OpenThermGasEstimate_Demo` keeps central heating and hot water on and prints the estimated power, energy, and gas every 10 s.
- `OpenThermGatewayMonitor_Demo` uses a slave object that faces the thermostat and a master object that faces the boiler. It passes each valid request to the boiler and passes the boiler's `SUCCESS` and `REJECTED` answers back. It prints requests with a `T` prefix and answers with a `B` prefix at 9600 baud, the rate that the OpenTherm Monitor app supports.

## Timing rules

- A master accepts an answer that ends within 1 s of the end of its request. The specification gives the slave 800 ms. The library judges a frame by when it arrived, so a late `process()` call does not change the result. An answer that is still arriving at 1 s is a `TIMEOUT`.
- After each conversation, a master waits 100 ms before it can send again. After a timeout, it waits 100 ms after the last change that it saw on the line, so a cut-off answer cannot overlap the next request.
- A slave waits 20 ms after the end of a request before `sendResponse()` sends.
- The receiver accepts up to 1.3 ms between the mid-bit transitions of a frame. The specification allows 0.9 to 1.15 ms, and the extra margin covers interrupt latency. A longer gap makes the frame `INVALID`.
- While it waits for a frame, the receiver ignores spikes shorter than 250 µs. A spike does not make the conversation `INVALID` and does not delay the timeout.

## Stop the library

`end()` detaches the interrupt. On a master, it also drives the line active. Section 3.5 of the specification lets a boiler read a line that stays at its low voltage, with no valid frame for 5 s, as a short circuit. The boiler treats a short circuit as a heat demand, and the active line prevents that. On a slave, `end()` leaves the output as it is.

If `end()` stops a conversation before `process()` has checked the answer, the library discards the answer. `sendRequest()` then returns 0, and the status stays `NONE`. After `end()`, `sendRequestAsync()` and `sendResponse()` return `false` until you call `begin()` again. `end()` does nothing if the object has not started or has already stopped. The destructor calls `end()`.

A copy of an `OpenTherm` object, such as one passed to a function by value, has the same pins and role but has not started. Destroying the copy does not stop the original.

## Changes from the original library

OpenThermPlus keeps the `OpenTherm` class and the `OpenTherm.h` header, so a sketch written for the original library compiles without changes. The one exception is a sketch that takes the address of `begin()`, or of a getter with `auto`, because these methods now have overloads and `begin()` returns `bool`. These changes can affect how it behaves:

- `OpenThermResponseStatus` has a new value, `REJECTED`. A `DATA_INVALID` or `UNKNOWN_DATA_ID` answer used to be `INVALID`.
- An answer with a different DATA-ID than its request is `INVALID`.
- On a master, a frame that would pass `isValidResponse()` comes back as 0 unless the status is `SUCCESS`. This includes an answer that arrives just before `end()`, which `sendRequest()` returns as 0.
- A frame with a stop bit of 0 or a missing mid-bit transition is `INVALID`. Spikes before a frame no longer make the conversation `INVALID`.
- `sendResponse()` called from the request callback of a slave now sends the answer. It used to return `false`.
- `buildRequest()` keeps the message type that you pass. It used to turn every type except `WRITE_DATA` into `READ_DATA`.
- `isValidRequest()` accepts `INVALID_DATA`.
- `getFault()` returns 0 when the answer is not valid, like the other getters.
- `end()` on a master drives the line active instead of leaving it as it was.
- `begin()` returns `false` and does not start when the input pin has no interrupt, or on AVR boards when the handler is `NULL`.
- `buildRequest()` and `buildResponse()` keep only the low 16 bits of `data`. On ESP boards, higher bits used to overwrite the DATA-ID and the message type.
- Destroying a copy of an `OpenTherm` object no longer detaches the interrupt of the original.

OpenThermPlus also adds names and overloads. The original names still work.

- `OpenThermRole::MASTER` and `OpenThermRole::SLAVE` for the constructor.
- `setCHSetpoint()` and `buildSetCHSetpointRequest()`, the new names of `setBoilerTemperature()` and `buildSetBoilerTemperatureRequest()`.
- Getters that take a variable, and `getFault(flags, oemCode)`.
- `isCentralHeating2Active()` and `floatToData()`.
- `getMaxCapacity()` and `OpenThermGasEstimator`.
- `CumulativeElectricityProduction`, the correct spelling of `CumulativElectricityProduction`.

## Run the tests

The tests build the library for your computer against a fake Arduino API in `test/stub`. They need a C++17 compiler, either `c++` or the one that `CXX` names.

```sh
test/run.sh
```

The script exits with a non-zero status if a test fails.

## License

This library started from [opentherm_library](https://github.com/ihormelnyk/opentherm_library) version 1.1.5 by [Ihor Melnyk](https://ihormelnyk.com). His [OpenTherm Library page](https://ihormelnyk.com/opentherm_library) describes the original design. The original code and the changes here are both under the MIT license. [`LICENSE`](LICENSE) keeps his copyright notice, as the license requires.
