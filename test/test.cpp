#include <OpenTherm.h>
#include <OpenThermGasEstimator.h>

#include <cmath>
#include <cstdio>
#include <string>

namespace
{
int failures = 0;

template <typename T>
std::string show(T value) { return std::to_string(value); }
std::string show(unsigned long value)
{
    char text[40];
    snprintf(text, sizeof text, "0x%08lx", value);
    return text;
}
std::string show(OpenThermResponseStatus status) { return OpenTherm::statusToString(status); }
std::string show(OpenThermMessageType type) { return OpenTherm::messageTypeToString(type); }
std::string show(OpenThermMessageID id) { return std::to_string(static_cast<int>(id)); }

void expect(bool ok, const char *message, int line)
{
    if (!ok)
    {
        printf("  FAIL line %d: %s\n", line, message);
        failures++;
    }
}

template <typename T, typename U>
void expectEq(const T &actual, const U &expected, const char *message, int line)
{
    if (!(actual == static_cast<T>(expected)))
    {
        printf("  FAIL line %d: %s: expected %s, got %s\n", line, message, show(static_cast<T>(expected)).c_str(), show(actual).c_str());
        failures++;
    }
}

#define EXPECT(ok, message) expect((ok), message, __LINE__)
#define EXPECT_EQ(actual, expected, message) expectEq((actual), (expected), message, __LINE__)

const int MASTER_IN = 2;
const int MASTER_OUT = 3;
const int SLAVE_IN = 4;
const int SLAVE_OUT = 5;

const unsigned long REQUEST = OpenTherm::buildRequest(OpenThermMessageType::READ_DATA, OpenThermMessageID::Tboiler, 0);
const unsigned long RESPONSE = OpenTherm::buildResponse(OpenThermMessageType::READ_ACK, OpenThermMessageID::Tboiler, 0x1234);

// Line level of each half bit of a frame: start bit, 32 data bits MSB first, stop bit. true = active.
std::vector<bool> halfBits(unsigned long frame)
{
    std::vector<bool> halves;
    auto bit = [&halves](bool one) {
        halves.push_back(one);
        halves.push_back(!one);
    };
    bit(true);
    for (int i = 31; i >= 0; i--)
        bit((frame >> i) & 1);
    bit(true);
    return halves;
}

void scheduleHalfBits(int pin, unsigned long start, const std::vector<bool> &halves, unsigned long bitPeriod = 1000)
{
    bool active = false;
    for (size_t i = 0; i < halves.size(); i++)
    {
        if (halves[i] != active)
        {
            active = halves[i];
            fake::at(start + i * bitPeriod / 2, pin, active ? HIGH : LOW);
        }
    }
    if (active)
        fake::at(start + halves.size() * bitPeriod / 2, pin, LOW);
}

void runFor(unsigned long us)
{
    const unsigned long end = fake::now + us;
    while (fake::now < end)
        yield();
}

unsigned long answer(unsigned long request)
{
    return OpenTherm::buildResponse(OpenThermMessageType::READ_ACK, OpenTherm::getDataID(request), 0x1234);
}

unsigned long faultWithBadParity(unsigned long request)
{
    return OpenTherm::buildResponse(OpenThermMessageType::READ_ACK, OpenTherm::getDataID(request), 0x0500) ^ (1ul << 31);
}

unsigned long silence(unsigned long)
{
    return 0;
}

unsigned long unknownDataId(unsigned long request)
{
    return OpenTherm::buildResponse(OpenThermMessageType::UNKNOWN_DATA_ID, OpenTherm::getDataID(request), 0x1234);
}

unsigned long dataInvalid(unsigned long request)
{
    return OpenTherm::buildResponse(OpenThermMessageType::DATA_INVALID, OpenTherm::getDataID(request), 0x1234);
}

unsigned long unknownDataIdWithBadParity(unsigned long request)
{
    return unknownDataId(request) ^ (1ul << 31);
}

unsigned long answerForTret(unsigned long)
{
    return OpenTherm::buildResponse(OpenThermMessageType::READ_ACK, OpenThermMessageID::Tret, 0x1234);
}

unsigned long answerZero(unsigned long request)
{
    return OpenTherm::buildResponse(OpenThermMessageType::READ_ACK, OpenTherm::getDataID(request), 0);
}

unsigned long answerFault(unsigned long request)
{
    return OpenTherm::buildResponse(OpenThermMessageType::READ_ACK, OpenTherm::getDataID(request), 0x0517);
}

// A master wired to a slave that answers either from its callback or from its loop once it is ready again.
struct Link
{
    OpenTherm master{MASTER_IN, MASTER_OUT};
    OpenTherm slave{SLAVE_IN, SLAVE_OUT, OpenThermRole::SLAVE};
    unsigned long (*respond)(unsigned long);
    bool fromCallback;
    unsigned long received = 0;
    unsigned long pending = 0;
    bool sent = false;
    int calls = 0;

    explicit Link(unsigned long (*respond)(unsigned long), bool fromCallback = false) : respond(respond), fromCallback(fromCallback)
    {
        fake::wire(MASTER_OUT, SLAVE_IN);
        fake::wire(SLAVE_OUT, MASTER_IN);
        slave.begin([this](unsigned long request, OpenThermResponseStatus status) {
            calls++;
            if (status != OpenThermResponseStatus::SUCCESS)
                return;
            received = request;
            if (this->fromCallback)
                sent = slave.sendResponse(this->respond(request));
            else
                pending = this->respond(request);
        });
        fake::onYield = [this] {
            slave.process();
            if (pending && slave.isReady())
            {
                slave.sendResponse(pending);
                pending = 0;
            }
        };
        master.begin();
    }
};

// The last write ends the frame most recently sent on outPin.
unsigned long lastWrite(int outPin)
{
    unsigned long time = 0;
    for (const fake::Write &write : fake::writes)
        if (write.pin == outPin)
            time = write.time;
    return time;
}

size_t writesTo(int pin)
{
    size_t count = 0;
    for (const fake::Write &write : fake::writes)
        if (write.pin == pin)
            count++;
    return count;
}

unsigned long firstActiveWriteAfter(int outPin, unsigned long after)
{
    for (const fake::Write &write : fake::writes)
        if (write.pin == outPin && write.time >= after && write.level == LOW)
            return write.time;
    return 0;
}

void framesHaveEvenParityAndTypeBits()
{
    const unsigned long frames[] = {
        OpenTherm::buildRequest(OpenThermMessageType::READ_DATA, OpenThermMessageID::Status, 0x0300),
        OpenTherm::buildRequest(OpenThermMessageType::WRITE_DATA, OpenThermMessageID::TSet, 0x4000),
        OpenTherm::buildResponse(OpenThermMessageType::WRITE_ACK, OpenThermMessageID::TSet, 0x4000),
        RESPONSE,
    };
    for (unsigned long frame : frames)
        EXPECT_EQ(__builtin_popcountl(frame) % 2, 0, "every built frame has an even number of 1 bits");
    EXPECT_EQ(RESPONSE >> 31, 1ul, "parity bit is set when the other 31 bits hold an odd count");

    EXPECT_EQ((OpenTherm::buildRequest(OpenThermMessageType::READ_DATA, OpenThermMessageID::Tboiler, 0) >> 28) & 7, 0b000ul, "READ_DATA type bits");
    EXPECT_EQ((OpenTherm::buildRequest(OpenThermMessageType::WRITE_DATA, OpenThermMessageID::TSet, 0) >> 28) & 7, 0b001ul, "WRITE_DATA type bits");
    EXPECT_EQ((OpenTherm::buildRequest(OpenThermMessageType::INVALID_DATA, OpenThermMessageID::TSet, 0) >> 28) & 7, 0b010ul, "INVALID_DATA type bits");

    EXPECT(OpenTherm::isValidResponse(RESPONSE), "READ_ACK with the parity bit set is a valid response");
    EXPECT(OpenTherm::isValidRequest(frames[1]), "WRITE_DATA with the parity bit set is a valid request");

    EXPECT_EQ(OpenTherm::getFloat(0xFF80), -0.5f, "f8.8 0xFF80 is -0.5");
    EXPECT_EQ(OpenTherm::getFloat(0xF340), -12.75f, "f8.8 0xF340 is -12.75");
}

void masterAndSlaveExchangeAFrame()
{
    Link link(answer);
    const unsigned long response = link.master.sendRequest(REQUEST);
    EXPECT_EQ(link.received, REQUEST, "slave decodes the master's request");
    EXPECT_EQ(link.master.getLastResponseStatus(), OpenThermResponseStatus::SUCCESS, "master accepts the slave's response");
    EXPECT_EQ(response, RESPONSE, "sendRequest() returns the slave's frame");
}

void slaveDecodesEachRequestOnItsOwn()
{
    Link link(silence);
    const unsigned long second = OpenTherm::buildRequest(OpenThermMessageType::WRITE_DATA, OpenThermMessageID::TSet, 0x4000);
    link.master.sendRequest(REQUEST);
    EXPECT_EQ(link.received, REQUEST, "slave decodes the first request");
    link.master.sendRequest(second);
    EXPECT_EQ(link.received, second, "no bits of the unanswered first request leak into the second");
}

// A master whose request went out and whose input line plays a scripted response.
struct ScriptedMaster
{
    OpenTherm ot{MASTER_IN, MASTER_OUT, OpenThermRole::MASTER};
    unsigned long sent;

    ScriptedMaster()
    {
        ot.begin();
        ot.sendRequestAsync(REQUEST);
        sent = fake::now;
    }

    OpenThermResponseStatus receive(const std::vector<bool> &halves, unsigned long bitPeriod = 1000)
    {
        scheduleHalfBits(MASTER_IN, sent + 50000, halves, bitPeriod);
        return finish();
    }

    OpenThermResponseStatus finish()
    {
        while (!ot.isReady() && fake::now - sent < 3000000)
        {
            ot.process();
            yield();
        }
        return ot.getLastResponseStatus();
    }
};

void unansweredRequestTimesOut()
{
    ScriptedMaster master;
    EXPECT_EQ(master.finish(), OpenThermResponseStatus::TIMEOUT, "no answer is a TIMEOUT");
    EXPECT(fake::now - master.sent > 1000000 && fake::now - master.sent < 1001000, "the timeout fires just after 1 s");
}

void lateProcessStillDeliversTheResponse()
{
    Link link(answer);
    EXPECT(link.master.sendRequestAsync(REQUEST), "request goes out");
    runFor(1500000);
    link.master.process();
    EXPECT_EQ(link.master.getLastResponseStatus(), OpenThermResponseStatus::SUCCESS, "a frame received long ago is still a success");
    EXPECT_EQ(link.master.getLastResponse(), RESPONSE, "and carries the slave's frame");
}

void lateProcessStillReportsAnInvalidFrame()
{
    OpenTherm master(MASTER_IN, MASTER_OUT);
    master.begin();
    master.sendRequestAsync(REQUEST);
    fake::at(fake::now + 50000, MASTER_IN, HIGH);
    fake::at(fake::now + 50800, MASTER_IN, LOW);
    runFor(1500000);
    master.process();
    EXPECT_EQ(master.getLastResponseStatus(), OpenThermResponseStatus::INVALID, "a start bit with an 800 us first half is reported as INVALID however late");
}

void frameStillArrivingAfterOneSecondTimesOut()
{
    ScriptedMaster master;
    scheduleHalfBits(MASTER_IN, master.sent + 990000, halfBits(RESPONSE));
    const unsigned long frameEnd = master.sent + 990000 + 34000;
    while (master.ot.getLastResponseStatus() == OpenThermResponseStatus::NONE)
    {
        master.ot.process();
        yield();
    }
    EXPECT_EQ(master.ot.getLastResponseStatus(), OpenThermResponseStatus::TIMEOUT, "a frame still arriving after 1 s is a TIMEOUT");
    EXPECT(fake::now - master.sent < 1001000, "reported at the deadline, not when the frame ends");
    master.finish();
    EXPECT(fake::now > frameEnd, "the master is not ready to send until the cut-off frame has ended");
}

void lateFrameIsATimeoutHoweverLateProcessRuns()
{
    ScriptedMaster master;
    scheduleHalfBits(MASTER_IN, master.sent + 980000, halfBits(RESPONSE));
    runFor(1500000);
    master.ot.process();
    EXPECT_EQ(master.ot.getLastResponseStatus(), OpenThermResponseStatus::TIMEOUT, "a frame that ended 1.0135 s after the request is a TIMEOUT");
    EXPECT_EQ(master.ot.getLastResponse(), 0ul, "and its acknowledgement is not passed on");
}

void frameEndingJustBeforeTheDeadlineSucceeds()
{
    ScriptedMaster master;
    scheduleHalfBits(MASTER_IN, master.sent + 966499, halfBits(RESPONSE));
    fake::advance(999998);
    fake::onInterruptsEnabled = [] { fake::advance(3); };
    master.ot.process();
    EXPECT_EQ(master.finish(), OpenThermResponseStatus::SUCCESS, "a stop bit at 999999 us delivered while process() masks interrupts is in time");
    EXPECT_EQ(master.ot.getLastResponse(), RESPONSE, "and the frame is kept");
}

void framesAtTheSpecBitPeriodLimitsDecode()
{
    for (unsigned long period : {900ul, 1150ul})
    {
        fake::reset();
        ScriptedMaster master;
        EXPECT_EQ(master.receive(halfBits(RESPONSE), period), OpenThermResponseStatus::SUCCESS, "a frame at a spec bit period decodes");
        EXPECT_EQ(master.ot.getLastResponse(), RESPONSE, "to the frame that was sent");
    }
}

void tooSlowFrameIsInvalid()
{
    ScriptedMaster master;
    EXPECT_EQ(master.receive(halfBits(RESPONSE), 1400), OpenThermResponseStatus::INVALID, "a 1400 us bit period is out of spec");
}

void missingMidBitTransitionIsInvalid()
{
    std::vector<bool> halves = halfBits(RESPONSE);
    const int bit = 20;
    halves[2 * (bit + 1) + 1] = halves[2 * (bit + 1)];
    ScriptedMaster master;
    EXPECT_EQ(master.receive(halves), OpenThermResponseStatus::INVALID, "a data bit without a mid-bit transition invalidates the frame");
}

void stopBitZeroIsInvalid()
{
    std::vector<bool> halves = halfBits(RESPONSE);
    halves[halves.size() - 2] = false;
    halves[halves.size() - 1] = true;
    ScriptedMaster master;
    EXPECT_EQ(master.receive(halves), OpenThermResponseStatus::INVALID, "a stop bit 0 invalidates the frame");
}

void masterIgnoresASpikeWhileWaiting()
{
    ScriptedMaster master;
    fake::at(master.sent + 10000, MASTER_IN, HIGH);
    fake::at(master.sent + 10100, MASTER_IN, LOW);
    EXPECT_EQ(master.receive(halfBits(RESPONSE)), OpenThermResponseStatus::SUCCESS, "a 100 us spike before the response is ignored");
    EXPECT_EQ(master.ot.getLastResponse(), RESPONSE, "and the real frame decodes");
}

void masterIgnoresAnIdleGoingEdgeWhileWaiting()
{
    fake::setLevel(MASTER_IN, HIGH);
    ScriptedMaster master;
    fake::at(master.sent + 10000, MASTER_IN, LOW);
    EXPECT_EQ(master.receive(halfBits(RESPONSE)), OpenThermResponseStatus::SUCCESS, "the line going idle before the response is ignored");
    EXPECT_EQ(master.ot.getLastResponse(), RESPONSE, "and the real frame decodes");
}

void spikesDoNotDeferTheTimeout()
{
    ScriptedMaster master;
    for (unsigned long t = 50000; t < 3000000; t += 100000)
    {
        fake::at(master.sent + t, MASTER_IN, HIGH);
        fake::at(master.sent + t + 100, MASTER_IN, LOW);
    }
    EXPECT_EQ(master.finish(), OpenThermResponseStatus::TIMEOUT, "a line carrying only spikes times out");
    EXPECT(fake::now - master.sent < 1100000, "within 1.1 s of the request");
}

void slaveIgnoresASpikeBeforeARequest()
{
    OpenTherm slave(SLAVE_IN, SLAVE_OUT, true);
    int calls = 0;
    unsigned long received = 0;
    OpenThermResponseStatus status = OpenThermResponseStatus::NONE;
    slave.begin([&](unsigned long request, OpenThermResponseStatus s) {
        calls++;
        received = request;
        status = s;
    });
    fake::onYield = [&slave] { slave.process(); };
    fake::at(fake::now + 10000, SLAVE_IN, HIGH);
    fake::at(fake::now + 10100, SLAVE_IN, LOW);
    scheduleHalfBits(SLAVE_IN, fake::now + 50000, halfBits(REQUEST));
    runFor(100000);
    EXPECT_EQ(calls, 1, "only the real request reaches the callback");
    EXPECT_EQ(status, OpenThermResponseStatus::SUCCESS, "the request after the spike is a success");
    EXPECT_EQ(received, REQUEST, "and decodes to the frame that was sent");
}

void slaveAnswersFromTheCallback()
{
    Link link(answer, true);
    const unsigned long response = link.master.sendRequest(REQUEST);
    EXPECT(link.sent, "sendResponse() called from the callback succeeds");
    EXPECT_EQ(link.calls, 1, "waiting out the delay does not call the callback again");
    EXPECT_EQ(link.master.getLastResponseStatus(), OpenThermResponseStatus::SUCCESS, "master accepts the answer");
    EXPECT_EQ(response, RESPONSE, "and gets the slave's frame");
    const unsigned long requestEnd = lastWrite(MASTER_OUT);
    const unsigned long answered = firstActiveWriteAfter(SLAVE_OUT, requestEnd);
    EXPECT(answered - requestEnd >= 20000, "the answer starts at least 20 ms after the request ends");
    EXPECT(answered - requestEnd < 21000, "and right after those 20 ms");
}

void masterWaitsAfterTheResponse()
{
    Link link(answer);
    link.master.sendRequest(REQUEST);
    const unsigned long responseEnd = lastWrite(SLAVE_OUT);
    while (!link.master.isReady())
    {
        link.master.process();
        yield();
    }
    EXPECT(link.master.sendRequestAsync(REQUEST), "the next request goes out once the master is ready");
    EXPECT(firstActiveWriteAfter(MASTER_OUT, responseEnd) - responseEnd >= 100000, "and starts at least 100 ms after the response ends");
}

void slaveAcceptsAnInvalidDataRequest()
{
    Link link(answer);
    const unsigned long request = OpenTherm::buildRequest(OpenThermMessageType::INVALID_DATA, OpenThermMessageID::Toutside, 0);
    link.master.sendRequest(request);
    EXPECT_EQ(link.received, request, "an INVALID-DATA request reaches the callback as a SUCCESS");
}

void refusalIsRejectedWithItsFrame()
{
    for (unsigned long (*refuse)(unsigned long) : {unknownDataId, dataInvalid})
    {
        fake::reset();
        Link link(refuse);
        EXPECT_EQ(link.master.sendRequest(REQUEST), refuse(REQUEST), "sendRequest() returns the refusal");
        EXPECT_EQ(link.master.getLastResponseStatus(), OpenThermResponseStatus::REJECTED, "a refusal is REJECTED");
        EXPECT_EQ(link.master.getBoilerTemperature(), 0.0f, "a getter does not read the data of a refusal");
    }
}

void refusalWithBadParityIsInvalid()
{
    Link link(unknownDataIdWithBadParity);
    EXPECT_EQ(link.master.sendRequest(REQUEST), unknownDataIdWithBadParity(REQUEST), "the frame keeps its bits for diagnostics");
    EXPECT_EQ(link.master.getLastResponseStatus(), OpenThermResponseStatus::INVALID, "a refusal with bad parity is INVALID");
}

void responseToAnotherDataIdIsInvalid()
{
    Link link(answerForTret);
    EXPECT_EQ(link.master.sendRequest(REQUEST), 0ul, "an acknowledgement for another DATA-ID is not returned");
    EXPECT_EQ(link.master.getLastResponseStatus(), OpenThermResponseStatus::INVALID, "and is INVALID");
    EXPECT_EQ(link.master.getBoilerTemperature(), 0.0f, "a getter does not read it");
}

void slaveReportsAFrameWithErrorsToItsCallback()
{
    OpenTherm slave(SLAVE_IN, SLAVE_OUT, true);
    int calls = 0;
    unsigned long received = 0;
    OpenThermResponseStatus status = OpenThermResponseStatus::NONE;
    slave.begin([&](unsigned long request, OpenThermResponseStatus s) {
        calls++;
        received = request;
        status = s;
    });
    fake::onYield = [&slave] { slave.process(); };
    std::vector<bool> halves = halfBits(REQUEST);
    halves[halves.size() - 2] = false;
    halves[halves.size() - 1] = true;
    scheduleHalfBits(SLAVE_IN, fake::now + 10000, halves);
    runFor(100000);
    EXPECT_EQ(calls, 1, "a request with a stop bit 0 reaches the callback once");
    EXPECT_EQ(status, OpenThermResponseStatus::INVALID, "as INVALID");
    EXPECT_EQ(received, REQUEST, "with the bits that were received");
}

void masterSendResponseDoesNotWait()
{
    Link link(answer);
    link.master.sendRequestAsync(REQUEST);
    while (link.master.getLastResponseStatus() == OpenThermResponseStatus::NONE)
    {
        link.master.process();
        yield();
    }
    EXPECT(!link.master.isReady(), "the master is in its 100 ms delay");
    const unsigned long before = fake::now;
    const size_t writes = writesTo(MASTER_OUT);
    EXPECT(!link.master.sendResponse(RESPONSE), "a master that is not ready refuses sendResponse()");
    EXPECT_EQ(fake::now, before, "without waiting");
    EXPECT_EQ(writesTo(MASTER_OUT), writes, "and without touching the line");
}

// A thermostat sends REQUEST through a gateway written like the gateway example.
OpenThermResponseStatus throughGateway(unsigned long (*boilerAnswer)(unsigned long), unsigned long &response)
{
    const int THERMOSTAT_IN = 6;
    const int THERMOSTAT_OUT = 7;
    const int GATEWAY_IN = 8;
    const int GATEWAY_OUT = 9;
    Link boiler(boilerAnswer);
    OpenTherm thermostat(THERMOSTAT_IN, THERMOSTAT_OUT);
    OpenTherm gateway(GATEWAY_IN, GATEWAY_OUT, true);
    fake::wire(THERMOSTAT_OUT, GATEWAY_IN);
    fake::wire(GATEWAY_OUT, THERMOSTAT_IN);
    bool relaying = false;
    gateway.begin([&](unsigned long request, OpenThermResponseStatus status) {
        if (status != OpenThermResponseStatus::SUCCESS)
            return;
        relaying = true;
        const unsigned long boilerResponse = boiler.master.sendRequest(request);
        const OpenThermResponseStatus boilerStatus = boiler.master.getLastResponseStatus();
        if (boilerStatus == OpenThermResponseStatus::SUCCESS || boilerStatus == OpenThermResponseStatus::REJECTED)
            gateway.sendResponse(boilerResponse);
        relaying = false;
    });
    const auto boilerLoop = fake::onYield;
    fake::onYield = [&] {
        boilerLoop();
        if (!relaying)
            gateway.process();
    };
    thermostat.begin();
    response = thermostat.sendRequest(REQUEST);
    return thermostat.getLastResponseStatus();
}

void gatewayRelaysTheBoilerAnswer()
{
    unsigned long response = 0;
    EXPECT_EQ(throughGateway(answer, response), OpenThermResponseStatus::SUCCESS, "the thermostat gets the boiler's answer through the gateway");
    EXPECT_EQ(response, RESPONSE, "unchanged");
    fake::reset();
    EXPECT_EQ(throughGateway(unknownDataId, response), OpenThermResponseStatus::REJECTED, "a refusal is relayed");
    EXPECT_EQ(response, unknownDataId(REQUEST), "unchanged");
    fake::reset();
    EXPECT_EQ(throughGateway(unknownDataIdWithBadParity, response), OpenThermResponseStatus::TIMEOUT, "a frame with errors is not relayed");
}

void getFaultIgnoresAnInvalidResponse()
{
    Link link(faultWithBadParity);
    EXPECT_EQ(link.master.getFault(), 0, "fault flags from a frame that failed its parity check are not reported");
    EXPECT_EQ(link.master.getLastResponseStatus(), OpenThermResponseStatus::INVALID, "the frame did arrive and was rejected");
}

void gettersIgnoreAFrameWithAFramingError()
{
    OpenTherm master(MASTER_IN, MASTER_OUT);
    master.begin();
    std::vector<bool> halves = halfBits(OpenTherm::buildResponse(OpenThermMessageType::READ_ACK, OpenThermMessageID::ASFflags, 0x0500));
    halves[halves.size() - 2] = false;
    halves[halves.size() - 1] = true;
    scheduleHalfBits(MASTER_IN, fake::now + 100000, halves);
    EXPECT_EQ(master.getFault(), 0, "fault flags from a frame whose stop bit is 0 are not reported");
    EXPECT_EQ(master.getLastResponseStatus(), OpenThermResponseStatus::INVALID, "the frame did arrive and was rejected");
}

void endedMasterHoldsTheLineActive()
{
    OpenTherm master(MASTER_IN, MASTER_OUT);
    master.begin();
    master.end();
    EXPECT_EQ(digitalRead(MASTER_OUT), LOW, "an ended master drives the line active");
    EXPECT(!master.sendRequestAsync(REQUEST), "and refuses to send");
}

void endedSlaveLeavesItsOutputAlone()
{
    OpenTherm slave(SLAVE_IN, SLAVE_OUT, true);
    slave.begin();
    const size_t before = writesTo(SLAVE_OUT);
    slave.end();
    EXPECT_EQ(writesTo(SLAVE_OUT), before, "end() does not touch a slave's output");
    EXPECT(!slave.sendResponse(RESPONSE), "and the slave refuses to send");
}

void endOnlyActsOnARunningObject()
{
    {
        OpenTherm neverBegun(MASTER_IN, MASTER_OUT);
        neverBegun.end();
    }
    EXPECT_EQ(writesTo(MASTER_OUT), 0u, "a master that never began leaves its pin alone, in end() and in its destructor");
    OpenTherm master(MASTER_IN, MASTER_OUT);
    master.begin();
    master.end();
    const size_t before = writesTo(MASTER_OUT);
    master.end();
    EXPECT_EQ(writesTo(MASTER_OUT), before, "a second end() leaves the pin alone");
}

void beginAfterEndWorks()
{
    Link link(answer);
    link.master.end();
    link.slave.end();
    link.slave.begin();
    link.master.begin();
    EXPECT_EQ(link.master.sendRequest(REQUEST), RESPONSE, "both sides talk again after end() and begin()");
}

void endFromTheCallbackReturnsFromSendRequest()
{
    Link link(answer);
    link.master.begin([&link](unsigned long, OpenThermResponseStatus) { link.master.end(); });
    EXPECT_EQ(link.master.sendRequest(REQUEST), RESPONSE, "sendRequest() returns the answer that arrived before end()");
}

void endBeforeProcessDiscardsTheFrame()
{
    const unsigned long otherId = OpenTherm::buildResponse(OpenThermMessageType::READ_ACK, OpenThermMessageID::Tret, 0x1234);
    for (unsigned long frame : {RESPONSE, otherId})
    {
        fake::reset();
        OpenTherm master(MASTER_IN, MASTER_OUT);
        master.begin();
        bool ended = false;
        fake::onYield = [&master, &ended, frame] {
            if (ended)
                return;
            ended = true;
            scheduleHalfBits(MASTER_IN, fake::now + 1000, halfBits(frame));
            fake::advance(40000);
            master.end();
        };
        float temperature = -1;
        EXPECT(!master.getBoilerTemperature(temperature), "a getter returns false when end() runs before process() checks the answer");
        EXPECT_EQ(temperature, -1.0f, "and leaves the argument unchanged");
        EXPECT_EQ(master.getLastResponse(), 0ul, "and the frame is discarded");
        EXPECT_EQ(master.getLastResponseStatus(), OpenThermResponseStatus::NONE, "and the status stays NONE");
    }
}

void builtFramesKeepDataInItsField()
{
    for (unsigned long frame : {OpenTherm::buildRequest(OpenThermMessageType::READ_DATA, OpenThermMessageID::Tboiler, 0x22345),
                                OpenTherm::buildResponse(OpenThermMessageType::READ_ACK, OpenThermMessageID::Tboiler, 0x22345)})
    {
        EXPECT_EQ(OpenTherm::getDataID(frame), OpenThermMessageID::Tboiler, "data above 16 bits does not change the DATA-ID");
        EXPECT_EQ(OpenTherm::getUInt(frame), 0x2345, "and is cut to 16 bits");
    }
    EXPECT_EQ(OpenTherm::getMessageType(OpenTherm::buildRequest(OpenThermMessageType::READ_DATA, OpenThermMessageID::Tboiler, 0x10000000)), OpenThermMessageType::READ_DATA, "nor the message type");
}

void floatToDataEncodesSignedValues()
{
    EXPECT_EQ(OpenTherm::floatToData(-5.25f), 0xFAC0, "-5.25 is 0xFAC0");
    EXPECT_EQ(OpenTherm::floatToData(21.3f), 0x154D, "21.3 rounds to the nearest 1/256");
    EXPECT_EQ(OpenTherm::floatToData(-21.3f), 0xEAB3, "and so does -21.3");
    EXPECT_EQ(OpenTherm::floatToData(1.0f / 512), 0x0001, "half of 1/256 rounds away from 0");
    EXPECT_EQ(OpenTherm::floatToData(-1.0f / 512), 0xFFFF, "on both sides");
    EXPECT_EQ(OpenTherm::floatToData(std::nextafter(1.0f / 512, 0.0f)), 0x0000, "just under half rounds to 0");
    EXPECT_EQ(OpenTherm::floatToData(std::nextafter(-1.0f / 512, 0.0f)), 0x0000, "on both sides");
    for (float value : {-128.0f, -40.5f, -0.5f, 0.0f, 64.0f, 127.99609375f})
        EXPECT_EQ(OpenTherm::getFloat(OpenTherm::floatToData(value)), value, "getFloat() decodes what floatToData() encodes");
    EXPECT_EQ(OpenTherm::floatToData(-200), 0x8000, "values below -128 encode as -128");
    EXPECT_EQ(OpenTherm::floatToData(200), 0x7FFF, "values above 127.996 encode as 127.996");
    EXPECT_EQ(OpenTherm::floatToData(NAN), 0x8000, "NaN encodes as -128");
}

void statusHelpersReadTheirOwnBits()
{
    bool (*const helpers[])(unsigned long) = {
        OpenTherm::isFault,
        OpenTherm::isCentralHeatingActive,
        OpenTherm::isHotWaterActive,
        OpenTherm::isFlameOn,
        OpenTherm::isCoolingActive,
        OpenTherm::isCentralHeating2Active,
        OpenTherm::isDiagnostic,
    };
    for (int helper = 0; helper < 7; helper++)
        for (int bit = 0; bit < 8; bit++)
            EXPECT_EQ(helpers[helper](1ul << bit), bit == helper, "each slave status helper reads only its bit");
}

void checkedGettersTellAZeroFromAFailure()
{
    struct Getter
    {
        bool (OpenTherm::*read)(float &);
        OpenThermMessageID id;
    };
    const Getter getters[] = {
        {&OpenTherm::getBoilerTemperature, OpenThermMessageID::Tboiler},
        {&OpenTherm::getReturnTemperature, OpenThermMessageID::Tret},
        {&OpenTherm::getDHWTemperature, OpenThermMessageID::Tdhw},
        {&OpenTherm::getModulation, OpenThermMessageID::RelModLevel},
        {&OpenTherm::getPressure, OpenThermMessageID::CHPressure},
    };
    for (const Getter &getter : getters)
    {
        fake::reset();
        Link link(answer);
        float value = -1;
        EXPECT((link.master.*getter.read)(value), "a getter returns true on SUCCESS");
        EXPECT_EQ(OpenTherm::getDataID(link.received), getter.id, "after reading its DATA-ID");
        EXPECT_EQ(value, 0x1234 / 256.0f, "and stores the f8.8 value");

        fake::reset();
        Link zero(answerZero);
        value = -1;
        EXPECT((zero.master.*getter.read)(value), "a real 0 is a success");
        EXPECT_EQ(value, 0.0f, "and is stored");

        fake::reset();
        Link mute(silence);
        value = -1;
        EXPECT(!(mute.master.*getter.read)(value), "a timeout returns false");
        EXPECT_EQ(value, -1.0f, "and leaves the argument unchanged");
    }
}

void getFaultReturnsTheFlagsAndTheOemCode()
{
    Link link(answerFault);
    unsigned char flags = 0;
    unsigned char oemCode = 0;
    EXPECT(link.master.getFault(flags, oemCode), "getFault() returns true on SUCCESS");
    EXPECT_EQ(OpenTherm::getDataID(link.received), OpenThermMessageID::ASFflags, "after reading ASFflags");
    EXPECT_EQ(flags, 0x05, "the high byte holds the flags");
    EXPECT_EQ(oemCode, 0x17, "the low byte holds the OEM fault code");
    EXPECT_EQ(link.master.getFault(), 0x05, "the old form returns the flags");
    fake::reset();
    Link bad(faultWithBadParity);
    flags = oemCode = 0x99;
    EXPECT(!bad.master.getFault(flags, oemCode), "a frame with bad parity returns false");
    EXPECT_EQ(flags, 0x99, "and leaves the flags unchanged");
    EXPECT_EQ(oemCode, 0x99, "and the OEM code");
}

void setCHSetpointWritesTSet()
{
    Link link(answer);
    EXPECT(link.master.setCHSetpoint(64), "setCHSetpoint() returns true when acknowledged");
    EXPECT_EQ(link.received, OpenTherm::buildRequest(OpenThermMessageType::WRITE_DATA, OpenThermMessageID::TSet, 0x4000), "and writes TSet");
    EXPECT(link.master.setBoilerTemperature(40), "setBoilerTemperature() still works");
    EXPECT_EQ(link.received, OpenTherm::buildRequest(OpenThermMessageType::WRITE_DATA, OpenThermMessageID::TSet, 0x2800), "and writes the same DATA-ID");
}

void checkedGettersFailWhenTheRequestCannotStart()
{
    Link link(answer);
    EXPECT_EQ(link.master.sendRequest(REQUEST), RESPONSE, "the first conversation succeeds");
    link.master.end();
    EXPECT_EQ(link.master.sendRequest(REQUEST), 0ul, "an ended master returns 0");
    EXPECT_EQ(link.master.getLastResponseStatus(), OpenThermResponseStatus::SUCCESS, "and the status still describes the first conversation");
    float temperature = -1;
    EXPECT(!link.master.getBoilerTemperature(temperature), "a checked getter returns false");
    EXPECT_EQ(temperature, -1.0f, "and leaves the argument unchanged");
    unsigned char flags = 0x99;
    unsigned char oemCode = 0x99;
    EXPECT(!link.master.getFault(flags, oemCode), "and so does getFault()");
}

void aRefusedRequestInTheCallbackKeepsTheOuterResult()
{
    Link link(answer);
    link.master.begin([&link](unsigned long, OpenThermResponseStatus) { link.master.sendRequest(REQUEST); });
    float temperature = -1;
    EXPECT(link.master.getBoilerTemperature(temperature), "a getter succeeds although its callback tried a request");
    EXPECT_EQ(temperature, 0x1234 / 256.0f, "and stores the answer");
    EXPECT_EQ(link.master.getLastResponseStatus(), OpenThermResponseStatus::SUCCESS, "and the status stays SUCCESS");
}

void beginRefusesAPinWithoutAnInterrupt()
{
    OpenTherm master(16, MASTER_OUT);
    EXPECT(!master.begin(), "begin() returns false when the input pin has no interrupt");
    EXPECT_EQ(writesTo(MASTER_OUT), 0u, "without touching the line");
    EXPECT(!master.sendRequestAsync(REQUEST), "and the master refuses to send");
    OpenTherm working(MASTER_IN, MASTER_OUT);
    EXPECT(working.begin(), "begin() returns true on an interrupt pin");
}

void destroyingACopyLeavesTheOriginalRunning()
{
    Link link(answer);
    {
        OpenTherm copy = link.master;
        EXPECT(!copy.sendRequestAsync(REQUEST), "a copy has not begun");
    }
    EXPECT_EQ(link.master.sendRequest(REQUEST), RESPONSE, "the original still receives after the copy is destroyed");
    EXPECT_EQ(digitalRead(MASTER_OUT), HIGH, "and its line is idle");
}

void aCopyKeepsThePinsAndTheRole()
{
    Link link(answer);
    link.slave.end();
    OpenTherm copy = link.slave;
    copy.begin([&copy](unsigned long request, OpenThermResponseStatus status) {
        if (status == OpenThermResponseStatus::SUCCESS)
            copy.sendResponse(answer(request));
    });
    fake::onYield = [&copy] { copy.process(); };
    EXPECT_EQ(link.master.sendRequest(REQUEST), RESPONSE, "a copy of the slave answers on the slave's pins");
}

void getMaxCapacityReadsBothBytes()
{
    Link link(answerFault);
    unsigned char maxCapacity = 0;
    unsigned char minModulation = 0;
    EXPECT(link.master.getMaxCapacity(maxCapacity, minModulation), "getMaxCapacity() returns true on SUCCESS");
    EXPECT_EQ(OpenTherm::getDataID(link.received), OpenThermMessageID::MaxCapacityMinModLevel, "after reading DATA-ID 15");
    EXPECT_EQ(maxCapacity, 0x05, "the high byte holds the capacity in kW");
    EXPECT_EQ(minModulation, 0x17, "the low byte holds the minimum modulation level");
    fake::reset();
    Link mute(silence);
    maxCapacity = minModulation = 0x99;
    EXPECT(!mute.master.getMaxCapacity(maxCapacity, minModulation), "a timeout returns false");
    EXPECT_EQ(maxCapacity, 0x99, "and leaves the capacity unchanged");
    EXPECT_EQ(minModulation, 0x99, "and the minimum modulation level");
}

bool near(float actual, float expected)
{
    return std::fabs(actual - expected) <= 1e-4f * std::fabs(expected) + 1e-6f;
}

void gasEstimatorMapsModulationBetweenMinAndMax()
{
    OpenThermGasEstimator gas(20, 25);
    gas.update(0, true, 0);
    EXPECT(near(gas.getPower(), 5), "0 % runs at the minimum modulation level");
    gas.update(0, true, 50);
    EXPECT(near(gas.getPower(), 12.5f), "50 % is halfway between the minimum and the maximum");
    gas.update(0, true, 100);
    EXPECT(near(gas.getPower(), 20), "100 % runs at the maximum");
    gas.update(0, true, 150);
    EXPECT(near(gas.getPower(), 20), "a level above 100 % counts as 100 %");
    gas.update(0, true, NAN);
    EXPECT(near(gas.getPower(), 5), "a NaN level counts as 0 %");
    gas.update(0, false, 100);
    EXPECT_EQ(gas.getPower(), 0.0f, "no flame burns no gas");
}

void gasEstimatorHoldsEachSampleUntilTheNext()
{
    OpenThermGasEstimator gas(24, 0, 10);
    gas.update(0, true, 50);
    EXPECT_EQ(gas.getEnergy(), 0.0f, "one sample has no duration yet");
    gas.update(30000, false, 0);
    EXPECT(near(gas.getEnergy(), 12 * 30 / 3600.0f), "the flame burned at 12 kW until the next sample");
    gas.update(60000, true, 100);
    EXPECT(near(gas.getEnergy(), 0.1f), "the time without flame adds nothing");
    EXPECT(near(gas.getGas(), 0.01f), "gas is energy over the calorific value");
}

void gasEstimatorSkipsAGapLongerThanAMinute()
{
    OpenThermGasEstimator gas(36);
    gas.update(0, true, 100);
    gas.update(OpenThermGasEstimator::maxSampleAge, true, 100);
    EXPECT(near(gas.getEnergy(), 0.6f), "a sample exactly a minute old still counts");
    gas.update(2 * OpenThermGasEstimator::maxSampleAge + 1, true, 100);
    EXPECT(near(gas.getEnergy(), 0.6f), "an older one does not");
}

void gasEstimatorSurvivesMillisWrapping()
{
    OpenThermGasEstimator gas(36);
    gas.update(0xffffffff - 4999, true, 100);
    gas.update(5000, true, 100);
    EXPECT(near(gas.getEnergy(), 0.1f), "10 s across the wrap of millis() count once");
}

void gasEstimatorAddsSmallStepsWithoutRounding()
{
    OpenThermGasEstimator gas(24);
    unsigned long now = 0;
    for (int second = 0; second <= 100 * 3600; second++, now += 1000)
        gas.update(now, true, 100);
    EXPECT(near(gas.getEnergy(), 2400), "100 h of 1 s samples at 24 kW make 2400 kWh");
}

static_assert(OpenThermMessageID::CumulativeElectricityProduction == OpenThermMessageID::CumulativElectricityProduction, "both spellings name DATA-ID 112");

struct Test
{
    const char *name;
    void (*run)();
};

const Test tests[] = {
    {"frames have even parity and the given type bits", framesHaveEvenParityAndTypeBits},
    {"master and slave exchange a frame end to end", masterAndSlaveExchangeAFrame},
    {"slave decodes each request on its own", slaveDecodesEachRequestOnItsOwn},
    {"unanswered request times out after 1 s", unansweredRequestTimesOut},
    {"late process() still delivers the response", lateProcessStillDeliversTheResponse},
    {"late process() still reports an invalid frame", lateProcessStillReportsAnInvalidFrame},
    {"a frame still arriving after 1 s times out", frameStillArrivingAfterOneSecondTimesOut},
    {"a late frame is a timeout however late process() runs", lateFrameIsATimeoutHoweverLateProcessRuns},
    {"a frame ending just before the deadline succeeds", frameEndingJustBeforeTheDeadlineSucceeds},
    {"frames at the spec bit period limits decode", framesAtTheSpecBitPeriodLimitsDecode},
    {"a too slow frame is invalid", tooSlowFrameIsInvalid},
    {"a missing mid-bit transition is invalid", missingMidBitTransitionIsInvalid},
    {"a stop bit 0 is invalid", stopBitZeroIsInvalid},
    {"master ignores a spike while waiting", masterIgnoresASpikeWhileWaiting},
    {"master ignores an idle-going edge while waiting", masterIgnoresAnIdleGoingEdgeWhileWaiting},
    {"spikes do not defer the timeout", spikesDoNotDeferTheTimeout},
    {"slave ignores a spike before a request", slaveIgnoresASpikeBeforeARequest},
    {"slave answers from the callback", slaveAnswersFromTheCallback},
    {"master waits 100 ms after the response", masterWaitsAfterTheResponse},
    {"slave accepts an INVALID-DATA request", slaveAcceptsAnInvalidDataRequest},
    {"a refusal is REJECTED with its frame", refusalIsRejectedWithItsFrame},
    {"a refusal with bad parity is INVALID", refusalWithBadParityIsInvalid},
    {"a response to another DATA-ID is INVALID", responseToAnotherDataIdIsInvalid},
    {"a slave reports a frame with errors to its callback", slaveReportsAFrameWithErrorsToItsCallback},
    {"a master's sendResponse() does not wait", masterSendResponseDoesNotWait},
    {"gateway relays the boiler answer", gatewayRelaysTheBoilerAnswer},
    {"getFault() ignores an invalid response", getFaultIgnoresAnInvalidResponse},
    {"getters ignore a frame with a framing error", gettersIgnoreAFrameWithAFramingError},
    {"an ended master holds the line active", endedMasterHoldsTheLineActive},
    {"an ended slave leaves its output alone", endedSlaveLeavesItsOutputAlone},
    {"end() only acts on a running object", endOnlyActsOnARunningObject},
    {"begin() after end() works", beginAfterEndWorks},
    {"end() from the callback returns from sendRequest()", endFromTheCallbackReturnsFromSendRequest},
    {"end() before process() discards the frame", endBeforeProcessDiscardsTheFrame},
    {"built frames keep data in its field", builtFramesKeepDataInItsField},
    {"floatToData() encodes signed values", floatToDataEncodesSignedValues},
    {"status helpers read their own bits", statusHelpersReadTheirOwnBits},
    {"checked getters tell a 0 from a failure", checkedGettersTellAZeroFromAFailure},
    {"getFault() returns the flags and the OEM code", getFaultReturnsTheFlagsAndTheOemCode},
    {"setCHSetpoint() writes TSet", setCHSetpointWritesTSet},
    {"checked getters fail when the request cannot start", checkedGettersFailWhenTheRequestCannotStart},
    {"a refused request in the callback keeps the outer result", aRefusedRequestInTheCallbackKeepsTheOuterResult},
    {"begin() refuses a pin without an interrupt", beginRefusesAPinWithoutAnInterrupt},
    {"destroying a copy leaves the original running", destroyingACopyLeavesTheOriginalRunning},
    {"a copy keeps the pins and the role", aCopyKeepsThePinsAndTheRole},
    {"getMaxCapacity() reads both bytes", getMaxCapacityReadsBothBytes},
    {"gas estimator maps modulation between min and max", gasEstimatorMapsModulationBetweenMinAndMax},
    {"gas estimator holds each sample until the next", gasEstimatorHoldsEachSampleUntilTheNext},
    {"gas estimator skips a gap longer than a minute", gasEstimatorSkipsAGapLongerThanAMinute},
    {"gas estimator survives millis() wrapping", gasEstimatorSurvivesMillisWrapping},
    {"gas estimator adds small steps without rounding", gasEstimatorAddsSmallStepsWithoutRounding},
};
} // namespace

int main()
{
    int failedTests = 0;
    for (const Test &test : tests)
    {
        fake::reset();
        const int before = failures;
        printf("%s\n", test.name);
        test.run();
        if (failures != before)
            failedTests++;
    }
    fake::reset();
    printf("%d of %zu tests failed\n", failedTests, sizeof tests / sizeof tests[0]);
    return failedTests == 0 ? 0 : 1;
}
