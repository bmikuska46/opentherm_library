/*
OpenTherm.cpp - OpenTherm Communication Library For Arduino, ESP8266, ESP32
Copyright 2023, Ihor Melnyk
*/

#include "OpenTherm.h"
#include <math.h>
#if !defined(__AVR__)
#include "FunctionalInterrupt.h"
#endif

namespace
{
constexpr unsigned long GLITCH_MAX_US = 250;
// The spec allows 900 to 1150 us between mid-bit transitions. Both bounds are widened by 150 us for interrupt latency.
constexpr unsigned long HALF_BIT_MAX_US = 750;
constexpr unsigned long BIT_PERIOD_MAX_US = 1300;
constexpr unsigned long RESPONSE_TIMEOUT_US = 1000000;
// DELAY starts at the stop bit's mid-bit transition, and the frame ends up to half a bit period later.
constexpr unsigned long STOP_BIT_TAIL_US = 575;
constexpr unsigned long SLAVE_RESPONSE_DELAY_US = 20000 + STOP_BIT_TAIL_US;
constexpr unsigned long MASTER_REQUEST_DELAY_US = 100000 + STOP_BIT_TAIL_US;

// Spec 4.4: every reply carries the DATA-ID of the request it answers.
OpenThermResponseStatus checkResponse(unsigned long response, OpenThermMessageID requestId)
{
    if (OpenTherm::parity(response) || OpenTherm::getDataID(response) != requestId)
        return OpenThermResponseStatus::INVALID;
    if (OpenTherm::isValidResponse(response))
        return OpenThermResponseStatus::SUCCESS;
    const OpenThermMessageType type = OpenTherm::getMessageType(response);
    return type == OpenThermMessageType::DATA_INVALID || type == OpenThermMessageType::UNKNOWN_DATA_ID ? OpenThermResponseStatus::REJECTED : OpenThermResponseStatus::INVALID;
}
}

OpenTherm::OpenTherm(int inPin, int outPin, bool isSlave) :
    status(OpenThermStatus::NOT_INITIALIZED),
    inPin(inPin),
    outPin(outPin),
    isSlave(isSlave),
    response(0),
    responseStatus(OpenThermResponseStatus::NONE),
    responseTimestamp(0),
    requestTimestamp(0),
    requestId(OpenThermMessageID::Status),
    processResponseCallback(NULL)
{
}

OpenTherm::OpenTherm(int inPin, int outPin, OpenThermRole role) :
    OpenTherm(inPin, outPin, role == OpenThermRole::SLAVE)
{
}

OpenTherm::OpenTherm(const OpenTherm &other) :
    OpenTherm(other.inPin, other.outPin, other.isSlave)
{
}

bool OpenTherm::begin(void (*handleInterruptCallback)(void))
{
    // Without an interrupt on inPin no frame is ever received.
#ifdef NOT_AN_INTERRUPT
    if (digitalPinToInterrupt(inPin) == NOT_AN_INTERRUPT)
        return false;
#endif
#if defined(__AVR__)
    if (handleInterruptCallback == NULL)
        return false;
#endif
    pinMode(inPin, INPUT);
    pinMode(outPin, OUTPUT);
    if (handleInterruptCallback != NULL)
    {
        attachInterrupt(digitalPinToInterrupt(inPin), handleInterruptCallback, CHANGE);
    }
    else
    {
#if !defined(__AVR__)
        attachInterruptArg(
            digitalPinToInterrupt(inPin),
            OpenTherm::handleInterruptHelper,
            this,
            CHANGE
        );
#endif
    }
    activateBoiler();
    status = OpenThermStatus::READY;
    return true;
}

bool OpenTherm::begin(void (*handleInterruptCallback)(void), void (*processResponseCallback)(unsigned long, OpenThermResponseStatus))
{
    if (!begin(handleInterruptCallback))
        return false;
    this->processResponseCallback = processResponseCallback;
    return true;
}

#if !defined(__AVR__)
bool OpenTherm::begin()
{
    return begin(NULL);
}

bool OpenTherm::begin(std::function<void(unsigned long, OpenThermResponseStatus)> processResponseFunction)
{
    if (!begin())
        return false;
    this->processResponseFunction = processResponseFunction;
    return true;
}
#endif

bool IRAM_ATTR OpenTherm::isReady()
{
    return status == OpenThermStatus::READY;
}

int IRAM_ATTR OpenTherm::readState()
{
    return digitalRead(inPin);
}

void OpenTherm::setActiveState()
{
    digitalWrite(outPin, LOW);
}

void OpenTherm::setIdleState()
{
    digitalWrite(outPin, HIGH);
}

void OpenTherm::activateBoiler()
{
    setIdleState();
    delay(1000);
}

void OpenTherm::sendBit(bool high)
{
    if (high)
        setActiveState();
    else
        setIdleState();
    delayMicroseconds(500);
    if (high)
        setIdleState();
    else
        setActiveState();
    delayMicroseconds(500);
}

bool OpenTherm::sendRequestAsync(unsigned long request)
{
    noInterrupts();
    const bool ready = isReady();

    if (!ready)
    {
        interrupts();
        return false;
    }

    status = OpenThermStatus::REQUEST_SENDING;
    response = 0;
    responseStatus = OpenThermResponseStatus::NONE;
    requestId = getDataID(request);

#ifdef INC_FREERTOS_H
    BaseType_t schedulerState = xTaskGetSchedulerState();
    if (schedulerState == taskSCHEDULER_RUNNING)
    {
        vTaskSuspendAll();
    }
#endif

    interrupts();

    sendBit(HIGH); // start bit
    for (int i = 31; i >= 0; i--)
    {
        sendBit(bitRead(request, i));
    }
    sendBit(HIGH); // stop bit
    setIdleState();

    requestTimestamp = micros();
    responseTimestamp = requestTimestamp;
    status = OpenThermStatus::RESPONSE_WAITING;

#ifdef INC_FREERTOS_H
    if (schedulerState == taskSCHEDULER_RUNNING) {
        xTaskResumeAll();
    }
#endif

    return true;
}

unsigned long OpenTherm::sendRequest(unsigned long request)
{
    if (!sendRequestAsync(request))
    {
        return 0;
    }

    // A callback that calls end() leaves NOT_INITIALIZED, which process() never leaves.
    while (!isReady() && status != OpenThermStatus::NOT_INITIALIZED)
    {
        process();
        yield();
    }
    return response;
}

bool OpenTherm::sendResponse(unsigned long response)
{
    // The request callback runs in DELAY, so an answer sent from it waits for process() to end the delay.
    while (isSlave && status == OpenThermStatus::DELAY)
    {
        process();
        yield();
    }

    noInterrupts();
    const bool ready = isReady();

    if (!ready)
    {
        interrupts();
        return false;
    }

    status = OpenThermStatus::REQUEST_SENDING;
    this->response = 0;
    responseStatus = OpenThermResponseStatus::NONE;

#ifdef INC_FREERTOS_H
    BaseType_t schedulerState = xTaskGetSchedulerState();
    if (schedulerState == taskSCHEDULER_RUNNING)
    {
        vTaskSuspendAll();
    }
#endif

    interrupts();

    sendBit(HIGH); // start bit
    for (int i = 31; i >= 0; i--)
    {
        sendBit(bitRead(response, i));
    }
    sendBit(HIGH); // stop bit
    setIdleState();
    status = OpenThermStatus::READY;

#ifdef INC_FREERTOS_H
    if (schedulerState == taskSCHEDULER_RUNNING) {
        xTaskResumeAll();
    }
#endif

    return true;
}

unsigned long OpenTherm::getLastResponse()
{
    return response;
}

OpenThermResponseStatus OpenTherm::getLastResponseStatus()
{
    return responseStatus;
}

void IRAM_ATTR OpenTherm::handleInterrupt()
{
    if (isReady())
    {
        if (isSlave && readState() == HIGH)
        {
            status = OpenThermStatus::RESPONSE_WAITING;
        }
        else
        {
            return;
        }
    }

    unsigned long newTs = micros();
    if (status == OpenThermStatus::RESPONSE_WAITING)
    {
        if (readState() == HIGH)
        {
            status = OpenThermStatus::RESPONSE_START_BIT;
            responseTimestamp = newTs;
        }
    }
    else if (status == OpenThermStatus::RESPONSE_START_BIT)
    {
        if ((newTs - responseTimestamp < GLITCH_MAX_US) && readState() == LOW)
        {
            status = isSlave ? OpenThermStatus::READY : OpenThermStatus::RESPONSE_WAITING;
        }
        else if ((newTs - responseTimestamp < HALF_BIT_MAX_US) && readState() == LOW)
        {
            status = OpenThermStatus::RESPONSE_RECEIVING;
            responseTimestamp = newTs;
            response = 0;
            responseBitIndex = 0;
        }
        else
        {
            status = OpenThermStatus::RESPONSE_INVALID;
            responseTimestamp = newTs;
        }
    }
    else if (status == OpenThermStatus::RESPONSE_RECEIVING)
    {
        if ((newTs - responseTimestamp) > BIT_PERIOD_MAX_US)
        {
            status = OpenThermStatus::RESPONSE_INVALID;
            responseTimestamp = newTs;
        }
        else if ((newTs - responseTimestamp) > HALF_BIT_MAX_US)
        {
            if (responseBitIndex < 32)
            {
                response = (response << 1) | !readState();
                responseTimestamp = newTs;
                responseBitIndex = responseBitIndex + 1;
            }
            else
            { // stop bit
                status = readState() == LOW ? OpenThermStatus::RESPONSE_READY : OpenThermStatus::RESPONSE_INVALID;
                responseTimestamp = newTs;
            }
        }
    }
}

#if !defined(__AVR__)
void IRAM_ATTR OpenTherm::handleInterruptHelper(void* ptr)
{
    static_cast<OpenTherm*>(ptr)->handleInterrupt();
}
#endif

void OpenTherm::processResponse()
{
    if (processResponseCallback != NULL)
    {
        processResponseCallback(response, responseStatus);
    }
#if !defined(__AVR__)
    if (this->processResponseFunction != NULL)
    {
        processResponseFunction(response, responseStatus);
    }
#endif
}

void OpenTherm::process()
{
    noInterrupts();
    OpenThermStatus st = status;
    unsigned long ts = responseTimestamp;
    // Read before interrupts resume, so a frame that completes after this snapshot completes after newTs.
    unsigned long newTs = micros();
    interrupts();

    if (st == OpenThermStatus::READY)
        return;
    OpenThermResponseStatus result;
    if (st == OpenThermStatus::RESPONSE_INVALID || st == OpenThermStatus::RESPONSE_READY)
    {
        status = OpenThermStatus::DELAY;
        if (isSlave)
            result = st == OpenThermStatus::RESPONSE_READY && isValidRequest(response) ? OpenThermResponseStatus::SUCCESS : OpenThermResponseStatus::INVALID;
        // A reply is judged by when it arrived, so a late process() call does not change the result.
        else if ((ts - requestTimestamp) > RESPONSE_TIMEOUT_US)
            result = OpenThermResponseStatus::TIMEOUT;
        else
            result = st == OpenThermStatus::RESPONSE_READY ? checkResponse(response, requestId) : OpenThermResponseStatus::INVALID;
    }
    else if (st == OpenThermStatus::DELAY)
    {
        if ((newTs - ts) > (isSlave ? SLAVE_RESPONSE_DELAY_US : MASTER_REQUEST_DELAY_US))
        {
            status = OpenThermStatus::READY;
        }
        return;
    }
    else if (st != OpenThermStatus::NOT_INITIALIZED && (newTs - (isSlave ? ts : requestTimestamp)) > RESPONSE_TIMEOUT_US)
    {
        // DELAY counts from the last transition, so a frame cut off here ends before the next request starts.
        status = OpenThermStatus::DELAY;
        result = OpenThermResponseStatus::TIMEOUT;
    }
    else
        return;

    // Callers check the returned frame with isValidResponse(), so on a master only a SUCCESS may pass it.
    if (!isSlave && result != OpenThermResponseStatus::SUCCESS && isValidResponse(response))
        response = 0;
    responseStatus = result;
    processResponse();
}

bool OpenTherm::parity(unsigned long frame) // odd parity
{
    byte p = 0;
    while (frame > 0)
    {
        if (frame & 1)
            p++;
        frame = frame >> 1;
    }
    return (p & 1);
}

OpenThermMessageType OpenTherm::getMessageType(unsigned long message)
{
    OpenThermMessageType msg_type = static_cast<OpenThermMessageType>((message >> 28) & 7);
    return msg_type;
}

OpenThermMessageID OpenTherm::getDataID(unsigned long frame)
{
    return (OpenThermMessageID)((frame >> 16) & 0xFF);
}

unsigned long OpenTherm::buildRequest(OpenThermMessageType type, OpenThermMessageID id, unsigned int data)
{
    // unsigned int has 32 bits on ESP boards, and higher bits would overwrite the DATA-ID and the message type.
    unsigned long request = data & 0xFFFF;
    request |= ((unsigned long)type) << 28;
    request |= ((unsigned long)id) << 16;
    if (parity(request))
        request |= (1ul << 31);
    return request;
}

unsigned long OpenTherm::buildResponse(OpenThermMessageType type, OpenThermMessageID id, unsigned int data)
{
    unsigned long response = data & 0xFFFF;
    response |= ((unsigned long)type) << 28;
    response |= ((unsigned long)id) << 16;
    if (parity(response))
        response |= (1ul << 31);
    return response;
}

bool OpenTherm::isValidResponse(unsigned long response)
{
    if (parity(response))
        return false;
    OpenThermMessageType msgType = getMessageType(response);
    return msgType == OpenThermMessageType::READ_ACK || msgType == OpenThermMessageType::WRITE_ACK;
}

bool OpenTherm::isValidRequest(unsigned long request)
{
    if (parity(request))
        return false;
    OpenThermMessageType msgType = getMessageType(request);
    return msgType == OpenThermMessageType::READ_DATA || msgType == OpenThermMessageType::WRITE_DATA || msgType == OpenThermMessageType::INVALID_DATA;
}

void OpenTherm::end()
{
    if (status == OpenThermStatus::NOT_INITIALIZED)
        return;
    detachInterrupt(digitalPinToInterrupt(inPin));
    // Only READY and DELAY come after process() has checked the frame, so in any other state the frame is not an answer yet.
    if (status != OpenThermStatus::READY && status != OpenThermStatus::DELAY)
        response = 0;
    if (!isSlave)
    {
        // Spec 3.5: an idle line with no valid frame for 5 s is a software short-circuit, which the boiler reads as heat demand.
        setActiveState();
    }
    status = OpenThermStatus::NOT_INITIALIZED;
}

OpenTherm::~OpenTherm()
{
    end();
}

const char *OpenTherm::statusToString(OpenThermResponseStatus status)
{
    switch (status)
    {
    case OpenThermResponseStatus::NONE:
        return "NONE";
    case OpenThermResponseStatus::SUCCESS:
        return "SUCCESS";
    case OpenThermResponseStatus::INVALID:
        return "INVALID";
    case OpenThermResponseStatus::TIMEOUT:
        return "TIMEOUT";
    case OpenThermResponseStatus::REJECTED:
        return "REJECTED";
    default:
        return "UNKNOWN";
    }
}

const char *OpenTherm::messageTypeToString(OpenThermMessageType message_type)
{
    switch (message_type)
    {
    case OpenThermMessageType::READ_DATA:
        return "READ_DATA";
    case OpenThermMessageType::WRITE_DATA:
        return "WRITE_DATA";
    case OpenThermMessageType::INVALID_DATA:
        return "INVALID_DATA";
    case OpenThermMessageType::RESERVED:
        return "RESERVED";
    case OpenThermMessageType::READ_ACK:
        return "READ_ACK";
    case OpenThermMessageType::WRITE_ACK:
        return "WRITE_ACK";
    case OpenThermMessageType::DATA_INVALID:
        return "DATA_INVALID";
    case OpenThermMessageType::UNKNOWN_DATA_ID:
        return "UNKNOWN_DATA_ID";
    default:
        return "UNKNOWN";
    }
}

// building requests

unsigned long OpenTherm::buildSetBoilerStatusRequest(bool enableCentralHeating, bool enableHotWater, bool enableCooling, bool enableOutsideTemperatureCompensation, bool enableCentralHeating2)
{
    unsigned int data = enableCentralHeating | (enableHotWater << 1) | (enableCooling << 2) | (enableOutsideTemperatureCompensation << 3) | (enableCentralHeating2 << 4);
    data <<= 8;
    return buildRequest(OpenThermMessageType::READ_DATA, OpenThermMessageID::Status, data);
}

unsigned long OpenTherm::buildSetCHSetpointRequest(float temperature)
{
    unsigned int data = temperatureToData(temperature);
    return buildRequest(OpenThermMessageType::WRITE_DATA, OpenThermMessageID::TSet, data);
}

unsigned long OpenTherm::buildSetBoilerTemperatureRequest(float temperature)
{
    return buildSetCHSetpointRequest(temperature);
}

unsigned long OpenTherm::buildGetBoilerTemperatureRequest()
{
    return buildRequest(OpenThermMessageType::READ_DATA, OpenThermMessageID::Tboiler, 0);
}

// parsing responses
bool OpenTherm::isFault(unsigned long response)
{
    return response & 0x1;
}

bool OpenTherm::isCentralHeatingActive(unsigned long response)
{
    return response & 0x2;
}

bool OpenTherm::isHotWaterActive(unsigned long response)
{
    return response & 0x4;
}

bool OpenTherm::isFlameOn(unsigned long response)
{
    return response & 0x8;
}

bool OpenTherm::isCoolingActive(unsigned long response)
{
    return response & 0x10;
}

bool OpenTherm::isCentralHeating2Active(unsigned long response)
{
    return response & 0x20;
}

bool OpenTherm::isDiagnostic(unsigned long response)
{
    return response & 0x40;
}

uint16_t OpenTherm::getUInt(const unsigned long response)
{
    const uint16_t u88 = response & 0xffff;
    return u88;
}

float OpenTherm::getFloat(const unsigned long response)
{
    const uint16_t u88 = getUInt(response);
    const float f = (u88 & 0x8000) ? -(0x10000L - u88) / 256.0f : u88 / 256.0f;
    return f;
}

unsigned int OpenTherm::temperatureToData(float temperature)
{
    if (temperature < 0)
        temperature = 0;
    if (temperature > 100)
        temperature = 100;
    unsigned int data = (unsigned int)(temperature * 256);
    return data;
}

unsigned int OpenTherm::floatToData(float value)
{
    if (value >= 0x7FFF / 256.0f)
        return 0x7FFF;
    // NaN fails every comparison, so a failed sensor read also becomes -128 instead of undefined behavior.
    if (!(value > -128))
        return 0x8000;
    return (unsigned int)(lround(value * 256) & 0xFFFF);
}

// basic requests

unsigned long OpenTherm::setBoilerStatus(bool enableCentralHeating, bool enableHotWater, bool enableCooling, bool enableOutsideTemperatureCompensation, bool enableCentralHeating2)
{
    return sendRequest(buildSetBoilerStatusRequest(enableCentralHeating, enableHotWater, enableCooling, enableOutsideTemperatureCompensation, enableCentralHeating2));
}

bool OpenTherm::setCHSetpoint(float temperature)
{
    unsigned long response = sendRequest(buildSetCHSetpointRequest(temperature));
    return isValidResponse(response);
}

bool OpenTherm::setBoilerTemperature(float temperature)
{
    return setCHSetpoint(temperature);
}

bool OpenTherm::setDHWSetpoint(float temperature)
{
    unsigned int data = temperatureToData(temperature);
    unsigned long response = sendRequest(buildRequest(OpenThermMessageType::WRITE_DATA, OpenThermMessageID::TdhwSet, data));
    return isValidResponse(response);
}

bool OpenTherm::readFloat(OpenThermMessageID id, float &value)
{
    // The frame, not getLastResponseStatus(), because a request that could not start leaves the status of the previous one.
    unsigned long response = sendRequest(buildRequest(OpenThermMessageType::READ_DATA, id, 0));
    if (!isValidResponse(response))
        return false;
    value = getFloat(response);
    return true;
}

float OpenTherm::readFloat(OpenThermMessageID id)
{
    float value = 0;
    readFloat(id, value);
    return value;
}

float OpenTherm::getBoilerTemperature()
{
    return readFloat(OpenThermMessageID::Tboiler);
}

bool OpenTherm::getBoilerTemperature(float &temperature)
{
    return readFloat(OpenThermMessageID::Tboiler, temperature);
}

float OpenTherm::getReturnTemperature()
{
    return readFloat(OpenThermMessageID::Tret);
}

bool OpenTherm::getReturnTemperature(float &temperature)
{
    return readFloat(OpenThermMessageID::Tret, temperature);
}

float OpenTherm::getDHWTemperature()
{
    return readFloat(OpenThermMessageID::Tdhw);
}

bool OpenTherm::getDHWTemperature(float &temperature)
{
    return readFloat(OpenThermMessageID::Tdhw, temperature);
}

float OpenTherm::getModulation()
{
    return readFloat(OpenThermMessageID::RelModLevel);
}

bool OpenTherm::getModulation(float &modulation)
{
    return readFloat(OpenThermMessageID::RelModLevel, modulation);
}

float OpenTherm::getPressure()
{
    return readFloat(OpenThermMessageID::CHPressure);
}

bool OpenTherm::getPressure(float &pressure)
{
    return readFloat(OpenThermMessageID::CHPressure, pressure);
}

unsigned char OpenTherm::getFault()
{
    unsigned char flags = 0;
    unsigned char oemCode = 0;
    getFault(flags, oemCode);
    return flags;
}

bool OpenTherm::getFault(unsigned char &flags, unsigned char &oemCode)
{
    unsigned long response = sendRequest(buildRequest(OpenThermMessageType::READ_DATA, OpenThermMessageID::ASFflags, 0));
    if (!isValidResponse(response))
        return false;
    flags = (response >> 8) & 0xff;
    oemCode = response & 0xff;
    return true;
}
