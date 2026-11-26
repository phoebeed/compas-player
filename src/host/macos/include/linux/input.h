/* macOS simulator stand-in for the Linux evdev header. There are no evdev
 * devices on a Mac, so only the constants and structs the player names are
 * here, with the same values as Linux. */
#pragma once
#include <stdint.h>
#include <sys/ioctl.h>
#include <sys/time.h>
struct input_event {
    struct timeval time;
    uint16_t type;
    uint16_t code;
    int32_t value;
};
#define EV_SYN 0x00
#define EV_KEY 0x01
#define EV_SW 0x05
#define SYN_REPORT 0
#define SYN_DROPPED 3
#define KEY_LEFT 105
#define KEY_RIGHT 106
#define KEY_DOWN 108
#define KEY_VOLUMEDOWN 114
#define KEY_VOLUMEUP 115
#define KEY_POWER 116
#define KEY_NEXTSONG 163
#define KEY_PLAYPAUSE 164
#define KEY_PREVIOUSSONG 165
#define KEY_REWIND 168
#define KEY_PLAYCD 200
#define KEY_PAUSECD 201
#define KEY_PLAY 207
#define KEY_FASTFORWARD 208
#define KEY_MAX 0x2ff
#define EVIOCGNAME(len) _IOC(IOC_OUT, 'E', 0x06, len)
#define EVIOCGKEY(len) _IOC(IOC_OUT, 'E', 0x18, len)
#define EVIOCGBIT(ev, len) _IOC(IOC_OUT, 'E', 0x20 + (ev), len)
#define EVIOCGRAB _IOW('E', 0x90, int)
