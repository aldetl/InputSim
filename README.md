# InputSim

InputSim is a Windows input-simulation library written in Rust. It provides a native DLL with a versioned C interface for resolving keyboard input and submitting individual keyboard or left mouse button transitions through Win32 `SendInput`.

Applications can use the DLL from C, C++, Rust, or another language with compatible foreign-function support. [Typing](https://github.com/aldetl/Typing) uses InputSim as its input-simulation component; InputSim can be built independently of Typing.

## Current scope

- Resolve Windows virtual keys using the foreground application's keyboard layout, including extended keys and left/right modifier identities.
- Resolve a character to a single key and its required Ctrl, Alt, and Shift modifiers when that layout can represent it.
- Submit one key or left mouse button down/up transition per call.
- Attach a caller-supplied, pointer-sized source tag to each submission.
- Return a result for each resolution or submission so the caller can account for partial sequences and failed releases.

The current implementation targets Windows x86 (32-bit) and x64 (64-bit). macOS and Linux backends are not implemented. Pointer movement, scrolling, right/middle/extra mouse button submission, keyboard interception, shortcut mapping, and automatic text sequencing are outside the current implementation. The additional mouse button constants in the header are reserved interface values; submitting those buttons returns `INPUTSIM_UNSUPPORTED_EVENT`.

## Build

### Windows x64 (64-bit)

For an x64 development build, use Windows with:

- A Rust `x86_64-pc-windows-msvc` host toolchain, including `rustfmt` and `clippy` for validation.
- Visual Studio or Build Tools with the MSVC C++ x64 tools and a Windows SDK.
- PowerShell for the validation launcher.

Run these commands from the repository root. Fetching provisions the locked dependencies; the build itself runs offline.

```powershell
cargo fetch --locked
cargo build --release --locked --offline
```

The DLL is written to `target/release/inputsim.dll`. Its public declarations are in [include/inputsim.h](include/inputsim.h). The client process and DLL must have matching architectures.

The default development build uses the Rust toolchain's default C runtime linkage. It does not establish a portable, clean-machine deployment package. Typing's packaging process builds its own matched InputSim DLL with static C runtime linkage and checks native dependencies.

### Windows x86 (32-bit)

For Windows 7 compatibility, the x86 build uses Rust's dedicated `i686-win7-windows-msvc` target, the pinned `nightly-2026-04-15` toolchain, and a rebuilt standard library. From the repository root:

```powershell
rustup toolchain install nightly-2026-04-15 --profile minimal --component rust-src
cargo +nightly-2026-04-15 build --release --locked --target i686-win7-windows-msvc -Z build-std=std,panic_unwind
```

These setup/build commands may download toolchain components and dependencies. The DLL is written to `target/i686-win7-windows-msvc/release/inputsim.dll`. Building the ordinary i686 Windows target does not establish Windows 7 compatibility. The standalone C validation launcher below is currently x64-only.

## Use the DLL

Load an `inputsim.dll` matching your application's architecture and resolve the functions declared in [inputsim.h](include/inputsim.h). [tools/windows_validate.c](tools/windows_validate.c) provides a working C loader and receiver.

| Function | Purpose | Success result |
| --- | --- | --- |
| `resolve_key_v1(virtual_key, output)` | Resolve a Windows virtual key to a native key identity without emitting input. | `INPUTSIM_RESOLVED` |
| `resolve_char_v1(codepoint, output)` | Resolve a Unicode scalar to a key and all required modifiers without emitting input. | `INPUTSIM_RESOLVED` |
| `submit_v1(event, direction, source_tag)` | Submit one resolved key or supported mouse button transition. | `INPUTSIM_INSERTED` |

Resolve a key, check the result, and submit down and up using the same saved event and source tag. Check every submission: an inserted down needs a matching successful up, even if focus or keyboard layout changes. InputSim does not track held keys, retry failed transitions, or release them automatically. The caller owns sequencing, cancellation, and cleanup.

For characters, the caller also coordinates the returned Ctrl/Alt/Shift modifiers and Caps Lock state. Character plans assume Caps Lock is off and exactly those modifiers are active. Unsupported mappings, including dead-key sequences and supplementary characters, are rejected. Before Windows 10 version 1607, stateless mapping checks provide weaker verification for unusual custom layouts and shifted dead keys.

`INPUTSIM_INSERTED` means insertion into the Windows input stream; it does not guarantee delivery to the intended application. Use the header's structures and result constants unchanged, pass valid aligned pointers, and keep the DLL loaded while calls are active. Source tags are pointer-sized; mouse hooks may expose only their low 32 bits.

## Validation

On an x64 MSVC host with the prerequisites and cached dependencies above, choose one validation mode:

```powershell
.\tools\validate_windows.ps1 -BuildOnly
.\tools\validate_windows.ps1
.\tools\validate_windows.ps1 -ReceiverOnly
```

- `-BuildOnly` checks formatting, unit tests, Clippy, the release build, C/C++ compatibility, DLL exports, and foreground-layout resolution. It needs an available Windows desktop but does not run live input delivery.
- With no switch, validation also runs live delivery checks on an isolated test desktop.
- `-ReceiverOnly` runs live checks with a dedicated receiver on the current desktop when desktop switching is unavailable.

Run live checks from an interactive Windows terminal with physical modifier keys and mouse buttons released. The tests restore test-owned foreground/pointer changes, and setup or delivery failures fail validation. Live results are written to `target/windows-validation.log`.

Windows 11 x64 is the available verification environment. Both x86 and x64 Rust tests have been checked there; the standalone receiver tool is x64-only. Actual Windows 7 SP1/Windows 10 x86 execution, clean-machine deployment, and isolated-desktop execution remain unverified in the recorded results.

## License

InputSim is licensed under the **Apache License, Version 2.0**. See [LICENSE](LICENSE) for the full terms. Its dependencies retain their respective licenses.
