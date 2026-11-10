#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../include/inputsim.h"

_Static_assert(sizeof(inputsim_event_v1) == 16, "event ABI size");
_Static_assert(_Alignof(inputsim_event_v1) == 4, "event ABI alignment");
_Static_assert(sizeof(inputsim_character_v1) == 68, "character ABI size");
_Static_assert(_Alignof(inputsim_character_v1) == 4, "character ABI alignment");
_Static_assert(sizeof(uintptr_t) == 8, "Windows x64 client required");

typedef uint32_t (__cdecl *resolve_key_fn)(uint32_t, inputsim_event_v1 *);
typedef uint32_t (__cdecl *resolve_char_fn)(uint32_t, inputsim_character_v1 *);
typedef uint32_t (__cdecl *submit_fn)(const inputsim_event_v1 *, uint32_t, uintptr_t);

static resolve_key_fn resolve_key;
static resolve_char_fn resolve_char;
static submit_fn submit;
static FILE *report;
static HWND receiver;
static POINT test_cursor;
static int cursor_moved;
static uintptr_t native_mouse_tag;
static const uintptr_t tag = UINT64_C(0x1234567800000001);

typedef struct observation {
    UINT message;
    DWORD scan;
    DWORD flags;
    uintptr_t tag;
} observation;

static observation keyboard_events[2048], mouse_events[64];
static unsigned keyboard_count, mouse_count, window_count, text_count;
static wchar_t received_text[512];
static int overflow;

typedef struct held_event {
    inputsim_event_v1 event;
    uintptr_t tag;
} held_event;

static held_event held[32];
static unsigned held_count;

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(report, "FAIL line %d: %s (Windows error %lu)\n", \
                __LINE__, #condition, GetLastError()); \
        return 0; \
    } \
} while (0)

static LRESULT CALLBACK keyboard_hook(int code, WPARAM message, LPARAM data) {
    if (code == HC_ACTION) {
        const KBDLLHOOKSTRUCT *event = (const KBDLLHOOKSTRUCT *)data;
        if (event->flags & LLKHF_INJECTED) {
            if (keyboard_count < ARRAYSIZE(keyboard_events)) {
                observation *entry = &keyboard_events[keyboard_count++];
                entry->message = (UINT)message;
                entry->scan = event->scanCode;
                entry->flags = event->flags;
                entry->tag = event->dwExtraInfo;
            } else {
                overflow = 1;
            }
        }
    }
    return CallNextHookEx(NULL, code, message, data);
}

static LRESULT CALLBACK mouse_hook(int code, WPARAM message, LPARAM data) {
    if (code == HC_ACTION && (message == WM_LBUTTONDOWN || message == WM_LBUTTONUP)) {
        const MSLLHOOKSTRUCT *event = (const MSLLHOOKSTRUCT *)data;
        if (event->flags & LLMHF_INJECTED) {
            if (mouse_count < ARRAYSIZE(mouse_events)) {
                observation *entry = &mouse_events[mouse_count++];
                entry->message = (UINT)message;
                entry->flags = event->flags;
                entry->tag = event->dwExtraInfo;
            } else {
                overflow = 1;
            }
        }
    }
    return CallNextHookEx(NULL, code, message, data);
}

static LRESULT CALLBACK receive(HWND window, UINT message, WPARAM key, LPARAM data) {
    switch (message) {
    case WM_KEYDOWN: case WM_KEYUP: case WM_SYSKEYDOWN: case WM_SYSKEYUP:
    case WM_LBUTTONDOWN: case WM_LBUTTONUP:
        window_count++;
        return 0;
    case WM_CHAR: case WM_SYSCHAR:
        if (text_count + 1 < ARRAYSIZE(received_text)) {
            received_text[text_count++] = (wchar_t)key;
            received_text[text_count] = L'\0';
        } else {
            overflow = 1;
        }
        return 0;
    case WM_CLOSE:
        return 0;
    default:
        return DefWindowProcW(window, message, key, data);
    }
}

static void pump(void) {
    MSG message;
    while (PeekMessageW(&message, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

static int wait_delivery(unsigned expected_window, unsigned expected_native, int mouse) {
    ULONGLONG deadline = GetTickCount64() + 1500;
    while (window_count < expected_window ||
           (mouse ? mouse_count : keyboard_count) < expected_native) {
        pump();
        if (GetTickCount64() >= deadline) {
            break;
        }
        MsgWaitForMultipleObjects(0, NULL, FALSE, 10, QS_ALLINPUT);
    }
    pump();
    CHECK(!overflow);
    CHECK(window_count == expected_window);
    CHECK((mouse ? mouse_count : keyboard_count) == expected_native);
    return 1;
}

static void account_for(const inputsim_event_v1 *event, uint32_t direction, uintptr_t source) {
    unsigned index;
    for (index = 0; index < held_count; index++) {
        if (memcmp(&held[index].event, event, sizeof(*event)) == 0 &&
            held[index].tag == source) {
            break;
        }
    }
    if (direction == INPUTSIM_DOWN && index == held_count) {
        if (held_count == ARRAYSIZE(held)) {
            overflow = 1;
            return;
        }
        held[held_count].event = *event;
        held[held_count++].tag = source;
    } else if (direction == INPUTSIM_UP && index < held_count) {
        held[index] = held[--held_count];
    }
}

static int emit(const inputsim_event_v1 *event, uint32_t direction, uintptr_t source) {
    int mouse = event->kind == INPUTSIM_MOUSE_BUTTON;
    unsigned native_index = mouse ? mouse_count : keyboard_count;
    unsigned receiver_index = window_count;
    uint32_t result;
    observation observed;
    CHECK(GetForegroundWindow() == receiver);
    result = submit(event, direction, source);
    fprintf(report, "submit kind=%u code=%u flags=%u direction=%u tag=%016llx result=%u\n",
            event->kind, event->code, event->flags, direction,
            (unsigned long long)source, result);
    CHECK(result == INPUTSIM_INSERTED);
    account_for(event, direction, source);
    CHECK(wait_delivery(receiver_index + 1, native_index + 1, mouse));
    observed = mouse ? mouse_events[native_index] : keyboard_events[native_index];
    fprintf(report, "observed message=%04x scan=%lu flags=%lu tag=%016llx\n",
            observed.message, observed.scan, observed.flags,
            (unsigned long long)observed.tag);
    CHECK(observed.tag == (mouse ? native_mouse_tag : source));
    if (mouse) {
        CHECK(observed.message == (UINT)(direction == INPUTSIM_UP ? WM_LBUTTONUP : WM_LBUTTONDOWN));
    } else {
        CHECK(!!(observed.flags & LLKHF_UP) == (direction == INPUTSIM_UP));
        if (event->kind == INPUTSIM_SCAN_CODE) {
            CHECK(observed.scan == event->code);
            // Windows can normalize right Shift's hook extended bit.
            if (event->code != 0x36) {
                CHECK(!!(observed.flags & LLKHF_EXTENDED) ==
                      !!(event->flags & INPUTSIM_EXTENDED));
            }
        }
    }
    return 1;
}

static int tap(const inputsim_event_v1 *event, uintptr_t source) {
    CHECK(emit(event, INPUTSIM_DOWN, source));
    CHECK(emit(event, INPUTSIM_UP, source));
    return 1;
}

static int key_for(UINT key, inputsim_event_v1 *event) {
    CHECK(resolve_key(key, event) == INPUTSIM_RESOLVED);
    CHECK(event->reserved == 0);
    return 1;
}

static int abi_failures(void) {
    inputsim_event_v1 event = {0};
    inputsim_character_v1 character;
    const inputsim_character_v1 empty = {0};
    CHECK(submit(NULL, INPUTSIM_DOWN, 0) == INPUTSIM_INVALID_EVENT);
    CHECK(resolve_key(VK_LEFT, NULL) == INPUTSIM_INVALID_EVENT);
    CHECK(resolve_char('A', NULL) == INPUTSIM_INVALID_EVENT);
    CHECK(resolve_key(0, &event) == INPUTSIM_INVALID_EVENT);
    CHECK(event.kind == 0 && event.code == 0 && event.flags == 0 && event.reserved == 0);
    memset(&character, 0xff, sizeof(character));
    CHECK(resolve_char(0xd800, &character) == INPUTSIM_INVALID_EVENT);
    CHECK(memcmp(&character, &empty, sizeof(empty)) == 0);
    CHECK(resolve_char(0x1f600, &character) == INPUTSIM_UNSUPPORTED_EVENT);
    CHECK(resolve_char(0x10ffff, &character) == INPUTSIM_UNSUPPORTED_EVENT);
    CHECK(submit(&event, INPUTSIM_DOWN, 0) == INPUTSIM_INVALID_EVENT);
    event.kind = INPUTSIM_MOUSE_BUTTON;
    event.code = INPUTSIM_MOUSE_RIGHT;
    CHECK(submit(&event, 0, 0) == INPUTSIM_INVALID_EVENT);
    CHECK(submit(&event, INPUTSIM_DOWN, 0) == INPUTSIM_UNSUPPORTED_EVENT);
    fprintf(report, "PASS C ABI invalid and unsupported requests\n");
    return 1;
}

static int physical_input_clear(void) {
    const int keys[] = {VK_LSHIFT, VK_RSHIFT, VK_LCONTROL, VK_RCONTROL,
                       VK_LMENU, VK_RMENU, VK_LWIN, VK_RWIN,
                       VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2};
    unsigned index;
    for (index = 0; index < ARRAYSIZE(keys); index++) {
        if (GetAsyncKeyState(keys[index]) & 0x8000) {
            fprintf(report, "FAIL physical input held, virtual key 0x%02x\n", keys[index]);
            return 0;
        }
    }
    return 1;
}

static int resolve_coverage(void) {
    const UINT keys[] = {
        VK_LEFT, VK_RIGHT, VK_UP, VK_DOWN, VK_PRIOR, VK_NEXT, VK_HOME, VK_END,
        VK_BACK, VK_DELETE, VK_CAPITAL, VK_TAB, VK_RETURN, VK_F4,
        VK_LCONTROL, VK_RCONTROL, VK_LMENU, VK_RMENU,
        VK_LSHIFT, VK_RSHIFT, VK_LWIN, VK_RWIN,
        VK_SHIFT, VK_CONTROL, VK_MENU, VK_PAUSE,
        VK_OEM_1, VK_OEM_PLUS, VK_OEM_COMMA, VK_OEM_MINUS, VK_OEM_PERIOD,
        VK_OEM_2, VK_OEM_3, VK_OEM_4, VK_OEM_5, VK_OEM_6, VK_OEM_7
    };
    inputsim_event_v1 event, left, right;
    unsigned index;
    UINT key;
    for (index = 0; index < ARRAYSIZE(keys); index++) {
        CHECK(key_for(keys[index], &event));
    }
    for (key = 'A'; key <= 'Z'; key++) {
        CHECK(key_for(key, &event));
    }
    for (key = '0'; key <= '9'; key++) {
        CHECK(key_for(key, &event));
    }
    for (key = VK_LSHIFT; key <= VK_LMENU; key += 2) {
        CHECK(key_for(key, &left));
        CHECK(key_for(key + 1, &right));
        CHECK(memcmp(&left, &right, sizeof(left)) != 0);
    }
    CHECK(key_for(VK_RIGHT, &event));
    CHECK(event.kind == INPUTSIM_SCAN_CODE && event.code == 0x4d &&
          event.flags == INPUTSIM_EXTENDED);
    fprintf(report, "PASS key coverage and Right Arrow identity\n");
    return 1;
}

static int coverage(void) {
    inputsim_event_v1 event;
    CHECK(resolve_coverage());
    CHECK(key_for(VK_RIGHT, &event));
    CHECK(tap(&event, tag));
    fprintf(report, "PASS Right Arrow delivery\n");
    return 1;
}

static int mouse_metadata_control(uintptr_t source) {
    inputsim_event_v1 event = {INPUTSIM_MOUSE_BUTTON, INPUTSIM_MOUSE_LEFT, 0, 0};
    uintptr_t observed_tag = 0;
    uint32_t direction;
    for (direction = INPUTSIM_DOWN; direction <= INPUTSIM_UP; direction++) {
        INPUT native = {0};
        unsigned native_index = mouse_count, receiver_index = window_count;
        CHECK(GetForegroundWindow() == receiver);
        native.type = INPUT_MOUSE;
        native.mi.dwFlags = direction == INPUTSIM_DOWN ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
        native.mi.dwExtraInfo = source;
        CHECK(SendInput(1, &native, sizeof(native)) == 1);
        account_for(&event, direction, source);
        CHECK(wait_delivery(receiver_index + 1, native_index + 1, 1));
        observed_tag = mouse_events[native_index].tag;
        CHECK(observed_tag == source || observed_tag == (uintptr_t)(uint32_t)source);
        if (direction == INPUTSIM_DOWN) {
            native_mouse_tag = observed_tag;
        } else {
            CHECK(observed_tag == native_mouse_tag);
        }
    }
    fprintf(report, "DIRECT SendInput mouse tag=%016llx observed=%016llx\n",
            (unsigned long long)source, (unsigned long long)observed_tag);
    return 1;
}

static int smoke(void) {
    inputsim_event_v1 event;
    RECT area;
    POINT point;
    CHECK(key_for(VK_LEFT, &event));
    CHECK(event.kind == INPUTSIM_SCAN_CODE && event.code == 0x4b &&
          event.flags == INPUTSIM_EXTENDED);
    CHECK(tap(&event, tag));
    CHECK(tap(&event, 0));
    CHECK(key_for(VK_LSHIFT, &event));
    CHECK(event.kind == INPUTSIM_SCAN_CODE && event.code == 0x2a && event.flags == 0);
    CHECK(tap(&event, tag + 1));
    CHECK(GetClientRect(receiver, &area));
    point.x = (area.right - area.left) / 2;
    point.y = (area.bottom - area.top) / 2;
    CHECK(ClientToScreen(receiver, &point));
    CHECK(SetCursorPos(point.x, point.y));
    test_cursor = point;
    cursor_moved = 1;
    event.kind = INPUTSIM_MOUSE_BUTTON;
    event.code = INPUTSIM_MOUSE_LEFT;
    event.flags = 0;
    CHECK(mouse_metadata_control(tag + 2));
    CHECK(tap(&event, tag + 2));
    fprintf(report, "PASS initial key and mouse smoke cases\n");
    return 1;
}

static int chords(void) {
    inputsim_event_v1 shift, arrow;
    unsigned index;
    CHECK(key_for(VK_RSHIFT, &shift));
    CHECK(key_for(VK_LEFT, &arrow));
    CHECK(emit(&shift, INPUTSIM_DOWN, tag));
    for (index = 0; index < 3; index++) {
        CHECK(tap(&arrow, tag));
    }
    CHECK(emit(&shift, INPUTSIM_UP, tag));
    fprintf(report, "PASS right Shift chord and repeated arrow taps\n");
    return 1;
}

static int type_character(wchar_t code) {
    inputsim_character_v1 plan;
    unsigned index;
    CHECK(resolve_char((uint32_t)code, &plan) == INPUTSIM_RESOLVED);
    CHECK(plan.modifier_count <= 3);
    for (index = 0; index < plan.modifier_count; index++) {
        CHECK(emit(&plan.modifiers[index], INPUTSIM_DOWN, tag));
    }
    CHECK(tap(&plan.key, tag));
    for (index = plan.modifier_count; index > 0; index--) {
        CHECK(emit(&plan.modifiers[index - 1], INPUTSIM_UP, tag));
    }
    return 1;
}

static int text_on_layout(HKL layout, const char *name) {
    const wchar_t *text = L"2026-11-07 11:30:45";
    const wchar_t *position;
    BYTE state[256] = {0};
    inputsim_character_v1 plan;
    CHECK(ActivateKeyboardLayout(layout, 0));
    CHECK(GetKeyboardLayout(0) == layout);
    CHECK(SetKeyboardState(state));
    text_count = 0;
    received_text[0] = L'\0';
    for (position = text; *position; position++) {
        CHECK(type_character(*position));
    }
    CHECK(wcscmp(received_text, text) == 0);
    CHECK(resolve_char(0x4e00, &plan) == INPUTSIM_UNSUPPORTED_EVENT);
    CHECK(plan.modifier_count == 0 && plan.key.kind == 0);
    fprintf(report, "PASS formatted date/time and unmappable character on %s\n", name);
    return 1;
}

static int altgr_character(HKL french) {
    inputsim_character_v1 plan;
    CHECK(ActivateKeyboardLayout(french, 0));
    CHECK(resolve_char('@', &plan) == INPUTSIM_RESOLVED);
    CHECK(plan.modifier_count == 2);
    CHECK(plan.modifiers[0].code == 0x1d && plan.modifiers[1].code == 0x38);
    text_count = 0;
    received_text[0] = L'\0';
    CHECK(type_character(L'@'));
    CHECK(wcscmp(received_text, L"@") == 0);
    fprintf(report, "PASS French Ctrl+Alt character and individual tagged results\n");
    return 1;
}

static int saved_identity(HKL us, HKL french) {
    inputsim_event_v1 saved, changed;
    CHECK(ActivateKeyboardLayout(us, 0));
    CHECK(key_for('A', &saved));
    CHECK(emit(&saved, INPUTSIM_DOWN, tag));
    CHECK(ActivateKeyboardLayout(french, 0));
    CHECK(key_for('A', &changed));
    CHECK(memcmp(&saved, &changed, sizeof(saved)) != 0);
    CHECK(emit(&saved, INPUTSIM_UP, tag));
    fprintf(report, "PASS saved key identity after foreground layout change\n");
    return 1;
}

static int cleanup_releases(void) {
    unsigned index = held_count;
    while (index > 0) {
        held_event *pending = &held[--index];
        uint32_t result = submit(&pending->event, INPUTSIM_UP, pending->tag);
        fprintf(report, "cleanup release result=%u\n", result);
        if (result == INPUTSIM_INSERTED) {
            account_for(&pending->event, INPUTSIM_UP, pending->tag);
        }
        pump();
    }
    if (held_count) {
        fprintf(report, "FAIL %u cleanup releases remain outstanding\n", held_count);
    }
    return held_count == 0;
}

static HMODULE load_api(const wchar_t *dll) {
    HMODULE library = LoadLibraryW(dll);
    FARPROC address;
    if (!library) {
        fprintf(report, "FAIL LoadLibrary error %lu\n", GetLastError());
        return NULL;
    }
#define LOAD(symbol, variable) \
    address = GetProcAddress(library, symbol); \
    if (!address) { \
        fprintf(report, "FAIL missing export %s\n", symbol); \
        FreeLibrary(library); \
        return NULL; \
    } \
    memcpy(&variable, &address, sizeof(variable))
    LOAD("resolve_key_v1", resolve_key);
    LOAD("resolve_char_v1", resolve_char);
    LOAD("submit_v1", submit);
#undef LOAD
    fprintf(report, "PASS exact DLL exports and C ABI sizes\n");
    return library;
}

static int abi_only(const wchar_t *dll) {
    HMODULE library;
    inputsim_character_v1 plan;
    int ok;
    report = stdout;
    library = load_api(dll);
    if (!library) return 1;
    ok = abi_failures() && resolve_coverage();
    if (ok) {
        ok = resolve_char('A', &plan) == INPUTSIM_RESOLVED &&
             resolve_char('1', &plan) == INPUTSIM_RESOLVED;
    }
    fprintf(report, "%s DLL ABI and foreground resolver checks\n", ok ? "PASS" : "FAIL");
    FreeLibrary(library);
    return ok ? 0 : 1;
}

static int installed_layout(HKL layout, const HKL *layouts, int count) {
    int index;
    for (index = 0; index < count; index++) {
        if (layouts[index] == layout) {
            return 1;
        }
    }
    return 0;
}

static int worker(const wchar_t *dll, const wchar_t *log) {
    HMODULE library = NULL;
    HHOOK keyboard = NULL, mouse = NULL;
    HKL original = GetKeyboardLayout(0), us = NULL, french = NULL;
    int had_us = 0, had_french = 0, ok = 0;
    HWND previous_window = GetForegroundWindow();
    POINT previous_cursor = {0}, current_cursor;
    int have_cursor = GetCursorPos(&previous_cursor);
    HKL initial_layouts[128];
    int layout_count = GetKeyboardLayoutList(ARRAYSIZE(initial_layouts), initial_layouts);
    WNDCLASSW window_class = {0};
    _wfopen_s(&report, log, L"w");
    if (!report) {
        return 2;
    }
    setvbuf(report, NULL, _IONBF, 0);
    library = load_api(dll);
    if (!library) {
        goto cleanup;
    }
    window_class.lpfnWndProc = receive;
    window_class.hInstance = GetModuleHandleW(NULL);
    window_class.lpszClassName = L"InputSimValidationReceiver";
    if (!RegisterClassW(&window_class)) {
        fprintf(report, "FAIL RegisterClass, error %lu\n", GetLastError());
        goto cleanup;
    }
    receiver = CreateWindowExW(0, window_class.lpszClassName,
        L"InputSim isolated validation", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        100, 100, 700, 400, NULL, NULL, window_class.hInstance, NULL);
    if (!receiver) {
        fprintf(report, "FAIL CreateWindow, error %lu\n", GetLastError());
        goto cleanup;
    }
    SetForegroundWindow(receiver);
    SetFocus(receiver);
    pump();
    keyboard = SetWindowsHookExW(WH_KEYBOARD_LL, keyboard_hook, window_class.hInstance, 0);
    mouse = SetWindowsHookExW(WH_MOUSE_LL, mouse_hook, window_class.hInstance, 0);
    if (!keyboard || !mouse) {
        fprintf(report, "FAIL receiver hooks error %lu\n", GetLastError());
        goto cleanup;
    }
    {
        ULONGLONG focus_deadline = GetTickCount64() + 60000;
        while (GetForegroundWindow() != receiver && GetTickCount64() < focus_deadline) {
            pump();
            MsgWaitForMultipleObjects(0, NULL, FALSE, 20, QS_ALLINPUT);
        }
        if (GetForegroundWindow() != receiver) {
            fprintf(report, "FAIL receiver focus unavailable\n");
            goto cleanup;
        }
    }
    us = LoadKeyboardLayoutW(L"00000409", 0);
    french = LoadKeyboardLayoutW(L"0000040C", 0);
    had_us = installed_layout(us, initial_layouts, layout_count);
    had_french = installed_layout(french, initial_layouts, layout_count);
    if (!us || !french) {
        fprintf(report, "FAIL US/French keyboard layouts unavailable\n");
        goto cleanup;
    }
    ok = ActivateKeyboardLayout(us, 0) && physical_input_clear() &&
         abi_failures() && smoke() && coverage() && chords() &&
         text_on_layout(us, "US") && text_on_layout(french, "French") &&
         altgr_character(french) && saved_identity(us, french);
cleanup:
    if (submit && !cleanup_releases()) {
        ok = 0;
    }
    if (!ActivateKeyboardLayout(original, 0)) {
        fprintf(report, "FAIL restore original layout, error %lu\n", GetLastError());
        ok = 0;
    }
    if (us && !had_us) {
        if (!UnloadKeyboardLayout(us)) {
            fprintf(report, "FAIL unload US test layout, error %lu\n", GetLastError());
            ok = 0;
        }
    }
    if (french && !had_french) {
        if (!UnloadKeyboardLayout(french)) {
            fprintf(report, "FAIL unload French test layout, error %lu\n", GetLastError());
            ok = 0;
        }
    }
    if (keyboard) UnhookWindowsHookEx(keyboard);
    if (mouse) UnhookWindowsHookEx(mouse);
    if (cursor_moved && have_cursor && GetCursorPos(&current_cursor) &&
        current_cursor.x == test_cursor.x && current_cursor.y == test_cursor.y) {
        if (!SetCursorPos(previous_cursor.x, previous_cursor.y)) {
            fprintf(report, "FAIL restore pointer, error %lu\n", GetLastError());
            ok = 0;
        }
    }
    if (receiver && GetForegroundWindow() == receiver && previous_window) {
        SetForegroundWindow(previous_window);
    }
    if (receiver) DestroyWindow(receiver);
    if (library) FreeLibrary(library);
    fprintf(report, "%s live Windows validation\n", ok ? "PASS" : "FAIL");
    fclose(report);
    return ok ? 0 : 1;
}

int wmain(int argc, wchar_t **argv) {
    HDESK original = NULL, isolated = NULL;
    STARTUPINFOW startup = {0};
    PROCESS_INFORMATION process = {0};
    wchar_t executable[MAX_PATH], dll[MAX_PATH], log[MAX_PATH], name[64], command[2048];
    DWORD code = 1;
    int restored = 1, switched = 0;
    FILE *results = NULL;
    if (argc == 4 && wcscmp(argv[1], L"--receiver") == 0) {
        return worker(argv[2], argv[3]);
    }
    if (argc == 3 && wcscmp(argv[1], L"--abi") == 0) {
        return abi_only(argv[2]);
    }
    if (argc != 3) {
        fwprintf(stderr, L"Usage: windows_validate.exe DLL LOG\n       windows_validate.exe --abi DLL\n       windows_validate.exe --receiver DLL LOG\n");
        return 2;
    }
    if (!GetModuleFileNameW(NULL, executable, ARRAYSIZE(executable)) ||
        !GetFullPathNameW(argv[1], ARRAYSIZE(dll), dll, NULL) ||
        !GetFullPathNameW(argv[2], ARRAYSIZE(log), log, NULL)) {
        return 2;
    }
    if (_wfopen_s(&results, log, L"w") != 0) {
        fprintf(stderr, "FAIL cannot create validation log\n");
        return 2;
    }
    fclose(results);
    swprintf_s(name, ARRAYSIZE(name), L"InputSimValidation-%lu", GetCurrentProcessId());
    original = OpenInputDesktop(0, FALSE, DESKTOP_SWITCHDESKTOP);
    if (!original) {
        fprintf(stderr, "FAIL OpenInputDesktop, error %lu\n", GetLastError());
        goto cleanup;
    }
    isolated = CreateDesktopW(name, NULL, NULL, 0, GENERIC_ALL, NULL);
    if (!isolated) {
        fprintf(stderr, "FAIL CreateDesktop, error %lu\n", GetLastError());
        goto cleanup;
    }
    if (!SwitchDesktop(isolated)) {
        fprintf(stderr, "FAIL SwitchDesktop, error %lu\n", GetLastError());
        goto cleanup;
    }
    switched = 1;
    startup.cb = sizeof(startup);
    startup.lpDesktop = name;
    swprintf_s(command, ARRAYSIZE(command), L"\"%s\" --receiver \"%s\" \"%s\"",
               executable, dll, log);
    if (!CreateProcessW(executable, command, NULL, NULL, FALSE, CREATE_NO_WINDOW,
                        NULL, NULL, &startup, &process)) {
        fprintf(stderr, "FAIL start validation worker, error %lu\n", GetLastError());
        goto cleanup;
    }
    if (WaitForSingleObject(process.hProcess, 90000) == WAIT_OBJECT_0) {
        GetExitCodeProcess(process.hProcess, &code);
    } else {
        fprintf(stderr, "FAIL validation worker timeout\n");
        TerminateProcess(process.hProcess, 1);
        WaitForSingleObject(process.hProcess, 5000);
    }
cleanup:
    if (switched) {
        restored = SwitchDesktop(original) != 0;
        if (!restored) fprintf(stderr, "FAIL restoring original desktop, error %lu\n", GetLastError());
    }
    if (process.hThread) CloseHandle(process.hThread);
    if (process.hProcess) CloseHandle(process.hProcess);
    if (isolated) CloseDesktop(isolated);
    if (original) CloseDesktop(original);
    if (_wfopen_s(&results, log, L"r") == 0 && results) {
        char line[1024];
        while (fgets(line, sizeof(line), results)) fputs(line, stdout);
        fclose(results);
    }
    return restored ? (int)code : 1;
}
