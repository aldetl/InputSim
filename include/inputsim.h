#ifndef INPUTSIM_H
#define INPUTSIM_H

#include <stdint.h>

#define INPUTSIM_SCAN_CODE     UINT32_C(1)
#define INPUTSIM_VIRTUAL_KEY   UINT32_C(2)
#define INPUTSIM_MOUSE_BUTTON  UINT32_C(3)
#define INPUTSIM_EXTENDED      UINT32_C(1)

#define INPUTSIM_MOUSE_LEFT    UINT32_C(1)
#define INPUTSIM_MOUSE_RIGHT   UINT32_C(2)
#define INPUTSIM_MOUSE_MIDDLE  UINT32_C(3)
#define INPUTSIM_MOUSE_X1      UINT32_C(4)
#define INPUTSIM_MOUSE_X2      UINT32_C(5)

#define INPUTSIM_DOWN UINT32_C(1)
#define INPUTSIM_UP   UINT32_C(2)

#define INPUTSIM_INSERTED          UINT32_C(0)
#define INPUTSIM_INVALID_EVENT     UINT32_C(1)
#define INPUTSIM_UNSUPPORTED_EVENT UINT32_C(2)
#define INPUTSIM_NOT_INSERTED      UINT32_C(3)
#define INPUTSIM_RESOLVED          UINT32_C(4)
#define INPUTSIM_UNAVAILABLE       UINT32_C(5)

typedef struct inputsim_event_v1 {
    uint32_t kind;
    uint32_t code;
    uint32_t flags;
    uint32_t reserved;
} inputsim_event_v1;

typedef struct inputsim_character_v1 {
    inputsim_event_v1 key;
    inputsim_event_v1 modifiers[3];
    uint32_t modifier_count;
} inputsim_character_v1;

#ifdef __cplusplus
extern "C" {
#endif

uint32_t __cdecl resolve_key_v1(uint32_t virtual_key,
                              inputsim_event_v1 *output);
uint32_t __cdecl resolve_char_v1(uint32_t codepoint,
                               inputsim_character_v1 *output);
uint32_t __cdecl submit_v1(const inputsim_event_v1 *event,
                         uint32_t direction, uintptr_t source_tag);

#ifdef __cplusplus
}
#endif

#endif
