# conpty.dll + OpenConsole.exe

These two binaries are Microsoft's Windows Terminal console host components
(from the [microsoft/terminal](https://github.com/microsoft/terminal) project,
MIT license), redistributed here via the
[pywinpty](https://github.com/spyder-ide/pywinpty) wheel (also MIT).

## Why they are needed

The inbox `conhost.exe` shipped with Windows 11 24H2/25H2 (build 26100+)
breaks the classic `CreatePseudoConsole` flow: a child spawned with
`PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE` either never attaches or is killed
right after attaching (exit code `0xC000013A`). Microsoft's own EchoCon
sample fails the same way. Windows Terminal, WezTerm, VS Code (node-pty)
and pywinpty all avoid this by hosting the terminal in their own bundled
`OpenConsole.exe` instead of the system conhost — this is that same
mechanism.

`LocalShellProcess` (src/terminal/LocalShellProcess_win.cpp) loads
`conpty.dll` from next to the executable at runtime and uses its
`ConptyCreatePseudoConsole` / `ConptyResizePseudoConsole` /
`ConptyClosePseudoConsole` exports. If the DLL is absent it falls back to
the system `CreatePseudoConsole`, which works on Windows 10 / older
Windows 11 builds.

Two additional requirements discovered while debugging (both mirrored from
pywinpty's winpty-rs):

1. The parent process must own a console — GUI apps allocate a hidden one
   (`AllocConsole` + `ShowWindow(SW_HIDE)`).
2. The parent's std handles must refer to that console at `CreateProcess`
   time, so `LocalShellProcess::start()` swaps them onto `CONOUT$`/`CONIN$`
   just for the spawn and restores them afterwards.

## Updating

Grab `conpty.dll` and `OpenConsole.exe` (matching architecture) from a
recent pywinpty wheel (`winpty/` directory inside the wheel) or build them
from the microsoft/terminal source. Keep both files side by side —
`conpty.dll` spawns `OpenConsole.exe` from its own directory.
