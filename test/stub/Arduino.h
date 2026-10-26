// Host fake of the Arduino API used by OpenTherm. The fake owns simulated time:
// micros() reads it and only delay(), delayMicroseconds(), yield() and fake::advance() move it.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <utility>
#include <vector>

typedef uint8_t byte;

#define LOW 0
#define HIGH 1
#define INPUT 0
#define OUTPUT 1
#define CHANGE 1
#define bitRead(value, bit) (((value) >> (bit)) & 0x01)

namespace fake
{
struct Write
{
    unsigned long time;
    int pin;
    int level;
};

inline unsigned long now = 0;
inline std::map<int, int> levels;
inline std::map<int, std::function<void()>> handlers;
inline std::map<int, int> wires; // out pin -> in pin
inline std::multimap<unsigned long, std::pair<int, int>> script; // time -> (in pin, level)
inline std::vector<Write> writes;
inline std::function<void()> onYield;
// Runs once at the next interrupts(), like an edge latched while interrupts were masked.
inline std::function<void()> onInterruptsEnabled;

inline void setLevel(int pin, int level)
{
    if (levels[pin] == level)
        return;
    levels[pin] = level;
    auto handler = handlers.find(pin);
    if (handler != handlers.end() && handler->second)
        handler->second();
}

inline void at(unsigned long time, int pin, int level)
{
    script.insert({time, {pin, level}});
}

inline void advance(unsigned long us)
{
    const unsigned long target = now + us;
    if (target > 60000000)
    {
        fprintf(stderr, "  ABORT: 60 s of simulated time passed, the code under test is stuck\n");
        abort();
    }
    while (!script.empty() && script.begin()->first <= target)
    {
        const auto event = *script.begin();
        script.erase(script.begin());
        if (event.first > now)
            now = event.first;
        setLevel(event.second.first, event.second.second);
    }
    now = target;
}

// Out LOW drives the line active, which the other side reads as HIGH.
inline void wire(int outPin, int inPin)
{
    wires[outPin] = inPin;
    levels[outPin] = HIGH;
    levels[inPin] = LOW;
}

inline void reset()
{
    now = 0;
    levels.clear();
    handlers.clear();
    wires.clear();
    script.clear();
    writes.clear();
    onYield = nullptr;
    onInterruptsEnabled = nullptr;
}
} // namespace fake

inline unsigned long micros() { return fake::now; }
inline void delayMicroseconds(unsigned int us) { fake::advance(us); }
inline void delay(unsigned long ms) { fake::advance(ms * 1000); }

inline void yield()
{
    fake::advance(10);
    if (fake::onYield)
        fake::onYield();
}

inline void noInterrupts() {}
inline void interrupts()
{
    if (!fake::onInterruptsEnabled)
        return;
    const auto deliver = fake::onInterruptsEnabled;
    fake::onInterruptsEnabled = nullptr;
    deliver();
}
inline void pinMode(int, int) {}
inline int digitalPinToInterrupt(int pin) { return pin; }
inline int digitalRead(int pin) { return fake::levels[pin]; }

inline void digitalWrite(int pin, int level)
{
    fake::writes.push_back({fake::now, pin, level});
    fake::levels[pin] = level;
    auto wire = fake::wires.find(pin);
    if (wire != fake::wires.end())
        fake::setLevel(wire->second, level == LOW ? HIGH : LOW);
}

inline void attachInterrupt(int pin, void (*handler)(void), int) { fake::handlers[pin] = handler; }
inline void attachInterruptArg(int pin, void (*handler)(void *), void *arg, int)
{
    fake::handlers[pin] = [handler, arg] { handler(arg); };
}
inline void detachInterrupt(int pin) { fake::handlers.erase(pin); }
