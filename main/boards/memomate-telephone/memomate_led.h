#ifndef _MEMOMATE_LED_H_
#define _MEMOMATE_LED_H_

#include "led/single_led.h"

// 灯语 for the headless telephone (no display — the LED is the only visual
// state channel). See the board README for the full table.
//
// Core rule: while the device is powered, the LED is NEVER dark. Idle shows
// a dim warm white, so "is it on?" is answerable at a glance; a dark LED
// means powered off. Solid = stable state, blinking = transient or
// needs-user-action, red is reserved exclusively for faults.
class MemomateLed : public SingleLed {
public:
    using SingleLed::SingleLed;

    void OnStateChanged() override;

    // Incoming-call 振铃 灯语 (ADR memomate-proactive-notification-ring §4):
    // a lively amber fast-blink that reads as "answer me". Driven directly by
    // the control connection — the device is Idle while ringing, so it doesn't
    // collide with the state-machine colors. Call OnStateChanged() to restore
    // the idle color when the ring stops without an answer.
    void ShowRinging();

    // Power-off feedback: LED goes dark right before the battery latch is
    // cut — the darkness itself is the "powered off" signal.
    void ShowPowerOff() { TurnOff(); }
};

#endif // _MEMOMATE_LED_H_
