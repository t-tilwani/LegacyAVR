## What does this change?

<!-- A short description, and the issue it fixes, e.g. "Fixes #12". -->

## Type of change

- [ ] Bug fix
- [ ] New feature
- [ ] Documentation
- [ ] Build / tooling
- [ ] Refactor (no behaviour change)

## How was it tested?

- [ ] `.\scripts\build.ps1 -RunTests` passes (native and .NET tests) on Windows 11 with VS 2026 / .NET 10
- [ ] Tested with real hardware (describe output type and AVR / TV):
- [ ] New or changed behaviour is covered by tests

## Checklist

- [ ] Real-time code (engine DSP thread) does not allocate, lock or block
- [ ] Telemetry layout / control protocol changes are mirrored in the C# side and pinned by tests (version bumped)
- [ ] User-facing changes are described in the in-app Settings guide and the README / docs
- [ ] No secrets, personal paths, logs, recordings or build output are included
- [ ] Third-party code or libraries added? Licence is compatible and `THIRD_PARTY_NOTICES.md` is updated
