#![cfg_attr(not(test), deny(warnings))]

#[cfg(not(all(
    target_os = "windows",
    any(target_arch = "x86_64", target_arch = "x86")
)))]
compile_error!("InputSim supports Windows x86 and x64.");

use std::mem::{size_of, zeroed};
use windows_sys::Win32::UI::Input::KeyboardAndMouse::*;
use windows_sys::Win32::UI::TextServices::HKL;
use windows_sys::Win32::UI::WindowsAndMessaging::{GetForegroundWindow, GetWindowThreadProcessId};

const SCAN_CODE: u32 = 1;
const VIRTUAL_KEY: u32 = 2;
const MOUSE_BUTTON: u32 = 3;
const EXTENDED: u32 = 1;
const DOWN: u32 = 1;
const UP: u32 = 2;
const INSERTED: u32 = 0;
const INVALID_EVENT: u32 = 1;
const UNSUPPORTED_EVENT: u32 = 2;
const NOT_INSERTED: u32 = 3;
const RESOLVED: u32 = 4;
const UNAVAILABLE: u32 = 5;

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Event {
    pub kind: u32,
    pub code: u32,
    pub flags: u32,
    pub reserved: u32,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Character {
    pub key: Event,
    pub modifiers: [Event; 3],
    pub modifier_count: u32,
}

fn foreground_layout() -> Result<HKL, u32> {
    // A zero thread ID would make GetKeyboardLayout use our own thread.
    unsafe {
        let window = GetForegroundWindow();
        if window == 0 {
            return Err(UNAVAILABLE);
        }
        let thread = GetWindowThreadProcessId(window, std::ptr::null_mut());
        if thread == 0 {
            return Err(UNAVAILABLE);
        }
        let layout = GetKeyboardLayout(thread);
        if layout == 0 {
            return Err(UNAVAILABLE);
        }
        Ok(layout)
    }
}

fn normalized_key(key: u32) -> u32 {
    match key as u16 {
        VK_SHIFT => VK_LSHIFT as u32,
        VK_CONTROL => VK_LCONTROL as u32,
        VK_MENU => VK_LMENU as u32,
        _ => key,
    }
}

fn mapped_key(key: u32, scan: u32) -> Event {
    let prefix = scan & 0xff00;
    // Navigation keys can map to a bare scan shared with a numpad key.
    let extended = prefix == 0xe000
        || matches!(
            key as u16,
            VK_PRIOR
                | VK_NEXT
                | VK_END
                | VK_HOME
                | VK_LEFT
                | VK_UP
                | VK_RIGHT
                | VK_DOWN
                | VK_INSERT
                | VK_DELETE
                | VK_RCONTROL
                | VK_RMENU
                | VK_DIVIDE
                | VK_NUMLOCK
                | VK_SNAPSHOT
                | VK_LWIN
                | VK_RWIN
                | VK_APPS
        );
    if scan == 0 || prefix == 0xe100 {
        Event {
            kind: VIRTUAL_KEY,
            code: key,
            flags: u32::from(prefix != 0xe100 && extended),
            ..Event::default()
        }
    } else {
        Event {
            kind: SCAN_CODE,
            code: scan & 0xff,
            flags: u32::from(extended),
            reserved: 0,
        }
    }
}

fn key_on_layout(key: u32, layout: HKL) -> Event {
    let key = normalized_key(key);
    let scan = unsafe { MapVirtualKeyExW(key, MAPVK_VK_TO_VSC_EX, layout) };
    mapped_key(key, scan)
}

fn character_plan(mapping: i16, mut resolve: impl FnMut(u32) -> Event) -> Result<Character, u32> {
    if mapping == -1 {
        return Err(UNSUPPORTED_EVENT);
    }
    let mapping = mapping as u16;
    let key = (mapping & 0xff) as u32;
    let shift = mapping >> 8;
    if !(1..=254).contains(&key) || shift & !7 != 0 {
        return Err(UNSUPPORTED_EVENT);
    }
    let mut output = Character {
        key: resolve(key),
        ..Character::default()
    };
    for (mask, modifier) in [(2, VK_LCONTROL), (4, VK_LMENU), (1, VK_LSHIFT)] {
        if shift & mask != 0 {
            output.modifiers[output.modifier_count as usize] = resolve(modifier as u32);
            output.modifier_count += 1;
        }
    }
    Ok(output)
}

fn character_on_layout(character: u16, layout: HKL) -> Result<Character, u32> {
    let mapping = unsafe { VkKeyScanExW(character, layout) };
    let plan = character_plan(mapping, |key| key_on_layout(key, layout))?;
    #[cfg(target_vendor = "win7")]
    let matches = if supports_state_preserving_translation()? {
        verify_character(character, layout, mapping, 4)
    } else {
        legacy_character_check(layout, mapping)
    };
    #[cfg(not(target_vendor = "win7"))]
    let matches = verify_character(character, layout, mapping, 4);
    if !matches {
        return Err(UNSUPPORTED_EVENT);
    }
    Ok(plan)
}

#[cfg(any(test, target_vendor = "win7"))]
fn legacy_character_check(layout: HKL, mapping: i16) -> bool {
    if mapping == -1 || (mapping as u16 >> 8) & !7 != 0 {
        return false;
    }
    let key = mapping as u16 & 0xff;
    let character = unsafe { MapVirtualKeyExW(key as u32, MAPVK_VK_TO_CHAR, layout) };
    character != 0 && character & 0x80000000 == 0
}

#[cfg(target_vendor = "win7")]
fn supports_state_preserving_translation() -> Result<bool, u32> {
    use windows_sys::Win32::System::{LibraryLoader::*, SystemInformation::OSVERSIONINFOW};
    let name: Vec<u16> = "ntdll.dll\0".encode_utf16().collect();
    let address = unsafe {
        GetProcAddress(
            GetModuleHandleW(name.as_ptr()),
            c"RtlGetVersion".as_ptr().cast(),
        )
    }
    .ok_or(UNAVAILABLE)?;
    let get: unsafe extern "system" fn(*mut OSVERSIONINFOW) -> i32 =
        unsafe { std::mem::transmute(address) };
    let mut version: OSVERSIONINFOW = unsafe { zeroed() };
    version.dwOSVersionInfoSize = size_of::<OSVERSIONINFOW>() as u32;
    if unsafe { get(&mut version) } < 0 {
        return Err(UNAVAILABLE);
    }
    Ok((
        version.dwMajorVersion,
        version.dwMinorVersion,
        version.dwBuildNumber,
    ) >= (10, 0, 14393))
}

fn verify_character(character: u16, layout: HKL, mapping: i16, flags: u32) -> bool {
    let key = (mapping as u16 & 0xff) as u32;
    let shift = mapping as u16 >> 8;
    let mut state = [0u8; 256];
    for (mask, modifier) in [(1, VK_SHIFT), (2, VK_CONTROL), (4, VK_MENU)] {
        if shift & mask != 0 {
            state[modifier as usize] = 0x80;
        }
    }
    let mut translated = [0u16; 8];
    // This path is used only where flag 4 preserves translation state.
    let count = unsafe {
        ToUnicodeEx(
            key,
            MapVirtualKeyExW(key, MAPVK_VK_TO_VSC_EX, layout),
            state.as_ptr(),
            translated.as_mut_ptr(),
            translated.len() as i32,
            flags,
            layout,
        )
    };
    count == 1 && translated[0] == character
}

// Centralize output initialization so every resolver failure clears its output.
unsafe fn resolve_into<T: Default>(
    output: *mut T,
    resolve: impl FnOnce() -> Result<T, u32>,
) -> u32 {
    if output.is_null() {
        return INVALID_EVENT;
    }
    output.write(T::default());
    match resolve() {
        Ok(value) => {
            output.write(value);
            RESOLVED
        }
        Err(result) => result,
    }
}

/// Resolve a Windows virtual key using the foreground keyboard layout.
///
/// # Safety
/// A nonnull output must reference a writable, aligned Event for this call.
#[no_mangle]
pub unsafe extern "C" fn resolve_key_v1(key: u32, output: *mut Event) -> u32 {
    resolve_into(output, || {
        if !(1..=254).contains(&key) {
            return Err(INVALID_EVENT);
        }
        Ok(key_on_layout(key, foreground_layout()?))
    })
}

/// Resolve a Unicode scalar to a key and all its required modifiers.
///
/// # Safety
/// A nonnull output must reference a writable, aligned Character for this call.
#[no_mangle]
pub unsafe extern "C" fn resolve_char_v1(codepoint: u32, output: *mut Character) -> u32 {
    resolve_into(output, || {
        let character = char::from_u32(codepoint).ok_or(INVALID_EVENT)?;
        if codepoint > 0xffff || character.is_control() {
            return Err(UNSUPPORTED_EVENT);
        }
        let layout = foreground_layout()?;
        character_on_layout(codepoint as u16, layout)
    })
}

fn native_input(event: Event, direction: u32, tag: usize) -> Result<INPUT, u32> {
    if event.reserved != 0 || !matches!(direction, DOWN | UP) {
        return Err(INVALID_EVENT);
    }
    let mut input: INPUT = unsafe { zeroed() };
    match event.kind {
        SCAN_CODE | VIRTUAL_KEY => {
            let max_code = if event.kind == SCAN_CODE { 255 } else { 254 };
            if !(1..=max_code).contains(&event.code) || event.flags & !EXTENDED != 0 {
                return Err(INVALID_EVENT);
            }
            input.r#type = INPUT_KEYBOARD;
            let mut keyboard: KEYBDINPUT = unsafe { zeroed() };
            if event.kind == SCAN_CODE {
                keyboard.wScan = event.code as u16;
                keyboard.dwFlags = KEYEVENTF_SCANCODE;
            } else {
                keyboard.wVk = event.code as u16;
            }
            if event.flags == EXTENDED {
                keyboard.dwFlags |= KEYEVENTF_EXTENDEDKEY;
            }
            if direction == UP {
                keyboard.dwFlags |= KEYEVENTF_KEYUP;
            }
            keyboard.dwExtraInfo = tag;
            input.Anonymous.ki = keyboard;
        }
        MOUSE_BUTTON => {
            if !(1..=5).contains(&event.code) || event.flags != 0 {
                return Err(INVALID_EVENT);
            }
            if event.code != 1 {
                return Err(UNSUPPORTED_EVENT);
            }
            input.r#type = INPUT_MOUSE;
            let mut mouse: MOUSEINPUT = unsafe { zeroed() };
            mouse.dwFlags = if direction == DOWN {
                MOUSEEVENTF_LEFTDOWN
            } else {
                MOUSEEVENTF_LEFTUP
            };
            mouse.dwExtraInfo = tag;
            input.Anonymous.mi = mouse;
        }
        _ => return Err(INVALID_EVENT),
    }
    Ok(input)
}

fn submit_with(event: Event, direction: u32, tag: usize, send: impl FnOnce(&INPUT) -> u32) -> u32 {
    let input = match native_input(event, direction, tag) {
        Ok(input) => input,
        Err(result) => return result,
    };
    if send(&input) == 1 {
        INSERTED
    } else {
        NOT_INSERTED
    }
}

/// Submit one resolved transition and return its Windows insertion result.
///
/// # Safety
/// A nonnull event must reference a readable, aligned Event for this call.
#[no_mangle]
pub unsafe extern "C" fn submit_v1(event: *const Event, direction: u32, tag: usize) -> u32 {
    if event.is_null() {
        return INVALID_EVENT;
    }
    submit_with(event.read(), direction, tag, |input| {
        SendInput(1, input, size_of::<INPUT>() as i32)
    })
}

#[cfg(test)]
mod tests;
