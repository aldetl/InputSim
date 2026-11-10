use super::*;
static LAYOUT_TEST_LOCK: std::sync::Mutex<()> = std::sync::Mutex::new(());

const TAG: usize = if usize::BITS == 64 {
    0x1234567800000001u64 as usize
} else {
    0x12340001
};

fn scan(code: u32, flags: u32) -> Event {
    Event {
        kind: SCAN_CODE,
        code,
        flags,
        reserved: 0,
    }
}

#[test]
fn c_layout_matches_header() {
    assert_eq!(size_of::<Event>(), 16);
    assert_eq!(std::mem::align_of::<Event>(), 4);
    assert_eq!(size_of::<Character>(), 68);
    assert_eq!(std::mem::align_of::<Character>(), 4);
    assert_eq!(size_of::<INPUT>(), if usize::BITS == 64 { 40 } else { 28 });
    assert_eq!(size_of::<usize>(), (usize::BITS / 8) as usize);
}

#[test]
fn native_key_identity_is_identical_for_down_and_up() {
    for event in [
        scan(0x4b, EXTENDED),
        scan(0x2a, 0),
        Event {
            kind: VIRTUAL_KEY,
            code: VK_PAUSE as u32,
            ..Event::default()
        },
        Event {
            kind: VIRTUAL_KEY,
            code: VK_RCONTROL as u32,
            flags: EXTENDED,
            reserved: 0,
        },
    ] {
        let down = native_input(event, DOWN, TAG).unwrap();
        let up = native_input(event, UP, TAG).unwrap();
        unsafe {
            assert_eq!(down.r#type, INPUT_KEYBOARD);
            let down = down.Anonymous.ki;
            let up = up.Anonymous.ki;
            assert_eq!(down.wVk, up.wVk);
            assert_eq!(down.wScan, up.wScan);
            assert_eq!(down.dwFlags | KEYEVENTF_KEYUP, up.dwFlags);
            assert_eq!(down.dwExtraInfo, TAG);
            assert_eq!(up.dwExtraInfo, TAG);
            assert_eq!(down.time, 0);
            if event.kind == VIRTUAL_KEY {
                assert_eq!(down.wVk, event.code as u16);
                assert_eq!(down.wScan, 0);
                assert_eq!(down.dwFlags & KEYEVENTF_SCANCODE, 0);
                assert_eq!(down.dwFlags & KEYEVENTF_EXTENDEDKEY, event.flags);
            }
        }
    }
    unsafe {
        let arrow = native_input(scan(0x4b, EXTENDED), DOWN, 0)
            .unwrap()
            .Anonymous
            .ki;
        assert_eq!(arrow.wVk, 0);
        assert_eq!(arrow.wScan, 0x4b);
        assert_eq!(arrow.dwExtraInfo, 0);
        assert_eq!(arrow.dwFlags, KEYEVENTF_SCANCODE | KEYEVENTF_EXTENDEDKEY);
    }
}

#[test]
fn mouse_maps_to_one_left_button_transition() {
    let event = Event {
        kind: MOUSE_BUTTON,
        code: 1,
        ..Event::default()
    };
    for (direction, flags) in [(DOWN, MOUSEEVENTF_LEFTDOWN), (UP, MOUSEEVENTF_LEFTUP)] {
        let native = native_input(event, direction, TAG).unwrap();
        assert_eq!(native.r#type, INPUT_MOUSE);
        unsafe {
            let mouse = native.Anonymous.mi;
            assert_eq!(
                (mouse.dx, mouse.dy, mouse.mouseData, mouse.time),
                (0, 0, 0, 0)
            );
            assert_eq!(mouse.dwFlags, flags);
            assert_eq!(mouse.dwExtraInfo, TAG);
        }
    }
}

#[test]
fn rejects_malformed_events_before_native_call() {
    let cases = [
        (Event::default(), DOWN),
        (scan(0, 0), DOWN),
        (scan(256, 0), DOWN),
        (scan(0x4b, 2), DOWN),
        (
            Event {
                reserved: 1,
                ..scan(0x4b, 0)
            },
            DOWN,
        ),
        (scan(0x4b, 0), 0),
        (
            Event {
                kind: VIRTUAL_KEY,
                code: 255,
                ..Event::default()
            },
            DOWN,
        ),
        (
            Event {
                kind: MOUSE_BUTTON,
                code: 0,
                ..Event::default()
            },
            DOWN,
        ),
        (
            Event {
                kind: MOUSE_BUTTON,
                code: 6,
                ..Event::default()
            },
            DOWN,
        ),
        (
            Event {
                kind: MOUSE_BUTTON,
                code: 2,
                flags: EXTENDED,
                reserved: 0,
            },
            DOWN,
        ),
        (
            Event {
                kind: MOUSE_BUTTON,
                code: 2,
                ..Event::default()
            },
            0,
        ),
    ];
    for (event, direction) in cases {
        assert_eq!(
            submit_with(event, direction, TAG, |_| panic!("native call")),
            INVALID_EVENT
        );
    }
    for code in 2..=5 {
        let event = Event {
            kind: MOUSE_BUTTON,
            code,
            ..Event::default()
        };
        assert_eq!(
            submit_with(event, DOWN, TAG, |_| panic!("native call")),
            UNSUPPORTED_EVENT
        );
    }
}

#[test]
fn failed_release_is_retried_only_by_the_caller() {
    let event = scan(0x2a, 0);
    let mut calls = 0;
    let mut submit = |direction, count| {
        submit_with(event, direction, TAG, |_| {
            calls += 1;
            count
        })
    };
    assert_eq!(submit(DOWN, 1), INSERTED);
    assert_eq!(submit(UP, 0), NOT_INSERTED);
    assert_eq!(submit(UP, 1), INSERTED);
    assert_eq!(calls, 3);
}

#[test]
fn resolves_scan_prefixes_and_generic_modifiers() {
    assert_eq!(mapped_key(VK_LEFT as u32, 0xe04b), scan(0x4b, EXTENDED));
    assert_eq!(mapped_key(VK_LEFT as u32, 0x4b), scan(0x4b, EXTENDED));
    assert_eq!(mapped_key(VK_NUMPAD4 as u32, 0x4b), scan(0x4b, 0));
    assert_eq!(mapped_key(VK_LSHIFT as u32, 0x2a), scan(0x2a, 0));
    for mapped in [0, 0xe11d] {
        assert_eq!(
            mapped_key(VK_PAUSE as u32, mapped),
            Event {
                kind: VIRTUAL_KEY,
                code: VK_PAUSE as u32,
                ..Event::default()
            }
        );
    }
    assert_eq!(normalized_key(VK_SHIFT as u32), VK_LSHIFT as u32);
    assert_eq!(normalized_key(VK_CONTROL as u32), VK_LCONTROL as u32);
    assert_eq!(normalized_key(VK_MENU as u32), VK_LMENU as u32);
    for key in [VK_RSHIFT, VK_RCONTROL, VK_RMENU, VK_RWIN] {
        assert_eq!(normalized_key(key as u32), key as u32);
    }
}

#[test]
fn character_plan_resolves_all_modifiers_before_output() {
    for shift in 0..=7 {
        let mut keys = Vec::new();
        let plan = character_plan((shift << 8) | 0x41, |key| {
            keys.push(key);
            Event {
                kind: VIRTUAL_KEY,
                code: key,
                ..Event::default()
            }
        })
        .unwrap();
        assert_eq!(plan.key.code, 0x41);
        let expected: Vec<_> = [(2, VK_LCONTROL), (4, VK_LMENU), (1, VK_LSHIFT)]
            .into_iter()
            .filter(|(mask, _)| shift & mask != 0)
            .map(|(_, key)| key as u32)
            .collect();
        assert_eq!(plan.modifier_count as usize, expected.len());
        assert_eq!(&keys[1..], expected);
        for (index, modifier) in expected.iter().enumerate() {
            assert_eq!(plan.modifiers[index].code, *modifier);
        }
        for modifier in &plan.modifiers[expected.len()..] {
            assert_eq!(*modifier, Event::default());
        }
    }
    for mapping in [-1, 0, 0x0841, 0x1041, 0x2041] {
        assert_eq!(
            character_plan(mapping, |_| panic!("resolve key")),
            Err(UNSUPPORTED_EVENT)
        );
    }
}

#[test]
fn resolver_errors_clear_output_and_validate_nulls() {
    unsafe {
        let mut key = scan(0x4b, EXTENDED);
        assert_eq!(resolve_key_v1(0, &mut key), INVALID_EVENT);
        assert_eq!(key, Event::default());
        assert_eq!(resolve_key_v1(256, &mut key), INVALID_EVENT);
        assert_eq!(resolve_key_v1(0x41, std::ptr::null_mut()), INVALID_EVENT);
        assert_eq!(resolve_char_v1(0x41, std::ptr::null_mut()), INVALID_EVENT);
        assert_eq!(submit_v1(std::ptr::null(), DOWN, 0), INVALID_EVENT);
        for (code, result) in [
            (0xd800, INVALID_EVENT),
            (0x110000, INVALID_EVENT),
            (0x1f600, UNSUPPORTED_EVENT),
            (0, UNSUPPORTED_EVENT),
            (10, UNSUPPORTED_EVENT),
        ] {
            let mut output = Character {
                key: scan(0x4b, 0),
                ..Character::default()
            };
            assert_eq!(resolve_char_v1(code, &mut output), result);
            assert_eq!(output, Character::default());
        }
        key = scan(0x4b, EXTENDED);
        assert_eq!(resolve_into(&mut key, || Err(UNAVAILABLE)), UNAVAILABLE);
        assert_eq!(key, Event::default());
    }
}

#[test]
fn windows_layout_mapping_handles_shift_altgr_and_dead_keys() {
    let _guard = LAYOUT_TEST_LOCK.lock().unwrap();
    struct Layout {
        handle: HKL,
        was_installed: bool,
    }
    impl Layout {
        fn load(name: &str) -> Self {
            unsafe {
                let count = GetKeyboardLayoutList(0, std::ptr::null_mut());
                let mut installed = vec![0; count as usize];
                GetKeyboardLayoutList(count, installed.as_mut_ptr());
                let name: Vec<u16> = name.encode_utf16().chain(Some(0)).collect();
                let handle = LoadKeyboardLayoutW(name.as_ptr(), 0);
                assert_ne!(handle, 0, "test keyboard layout unavailable");
                Self {
                    handle,
                    was_installed: installed.contains(&handle),
                }
            }
        }
    }
    impl Drop for Layout {
        fn drop(&mut self) {
            if !self.was_installed {
                unsafe {
                    UnloadKeyboardLayout(self.handle);
                }
            }
        }
    }
    let us = Layout::load("00000409");
    let french = Layout::load("0000040C");
    assert_eq!(
        key_on_layout(VK_RIGHT as u32, us.handle),
        scan(0x4d, EXTENDED)
    );
    let saved = key_on_layout('A' as u32, us.handle);
    assert_ne!(saved, key_on_layout('A' as u32, french.handle));
    assert_eq!(saved, scan(0x1e, 0));
    assert_eq!(
        character_on_layout('1' as u16, us.handle)
            .unwrap()
            .modifier_count,
        0
    );
    let digit = character_on_layout('1' as u16, french.handle).unwrap();
    assert_eq!(digit.modifier_count, 1);
    assert_eq!(digit.modifiers[0], scan(0x2a, 0));
    let at = character_on_layout('@' as u16, french.handle).unwrap();
    assert_eq!(at.modifier_count, 2);
    assert_eq!(at.modifiers[0], scan(0x1d, 0));
    assert_eq!(at.modifiers[1], scan(0x38, 0));
    assert_eq!(
        character_on_layout('^' as u16, french.handle),
        Err(UNSUPPORTED_EVENT)
    );
    // Rejecting a dead key must not poison later character mapping.
    assert!(character_on_layout('e' as u16, french.handle).is_ok());
    for layout in [us.handle, french.handle] {
        for character in "2026-11-07 11:30:45".encode_utf16() {
            assert!(character_on_layout(character, layout).is_ok());
        }
        assert_eq!(character_on_layout(0x4e00, layout), Err(UNSUPPORTED_EVENT));
    }
}

#[test]
fn stateless_legacy_character_check_preserves_callers_dead_key_state() {
    let _guard = LAYOUT_TEST_LOCK.lock().unwrap();
    // The Rust test thread has its own input queue, independent of user apps.
    // Only unload a layout that this test itself added.
    let mut existing = [0isize; 64];
    let count = unsafe { GetKeyboardLayoutList(existing.len() as i32, existing.as_mut_ptr()) };
    let name: Vec<u16> = "0000040c\0".encode_utf16().collect();
    let layout = unsafe { LoadKeyboardLayoutW(name.as_ptr(), 0) };
    assert_ne!(
        layout, 0,
        "French layout is needed for dead-key isolation verification"
    );
    struct AddedLayout(HKL, bool);
    impl Drop for AddedLayout {
        fn drop(&mut self) {
            if self.1 {
                unsafe {
                    UnloadKeyboardLayout(self.0);
                }
            }
        }
    }
    let _layout = AddedLayout(layout, !existing[..count.max(0) as usize].contains(&layout));
    let state = [0u8; 256];
    let translate = |key: u32, flags: u32| {
        let mut text = [0u16; 8];
        let count = unsafe {
            ToUnicodeEx(
                key,
                MapVirtualKeyExW(key, MAPVK_VK_TO_VSC_EX, layout),
                state.as_ptr(),
                text.as_mut_ptr(),
                8,
                flags,
                layout,
            )
        };
        (count, text[0])
    };
    let dead = (1..=254)
        .find(|key| translate(*key, 4).0 < 0)
        .expect("French dead key");
    assert!(translate(dead, 0).0 < 0);
    let character = 'a' as u16;
    let mapping = unsafe { VkKeyScanExW(character, layout) };
    let matches = legacy_character_check(layout, mapping);
    let after = translate(VK_SPACE as u32, 0);
    assert!(matches);
    // Stateless resolution leaves the caller's pending accent available to Space.
    assert_eq!(after.0, 1);
    assert_ne!(after.1, ' ' as u16);
    translate(VK_SPACE as u32, 0); // Leave this test thread's buffer empty.
    assert!(!legacy_character_check(layout, dead as i16));
}
