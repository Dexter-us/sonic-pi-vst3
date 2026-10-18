# Dexter U.S. Sonic Pi VST3 live-coding instrument

This prototype combines the upstream Sonic Pi Ruby runtime with the
SuperSonic/scsynth engine in a loadable VST3. Its VSTGUI editor contains a
multi-line Sonic Pi code editor and Run/Stop controls. The audio callback
renders directly into the host's stereo output buffers; a separate worker runs
the Sonic Pi language runtime. It is registered as an Instrument/Synth by
Dexter U.S., and the editor header carries the same branding.

## Scope and current limitations

- The plug-in does not read DAW tempo or transport. Sonic Pi gets a free-running
  internal clock, initially 60 BPM, so its own scheduler can run independently
  of the host.
- There is no VST MIDI input or output path. MIDI-related Sonic Pi APIs are not
  bridged by this prototype.
- Sonic Pi's file-based buffer loads are decoded and copied into Clockwork
  buffers by the OSC adapter. The `supersonic-audio-out` device synth is not
  used; audio is delivered through the host's output buffers.
- The embedded engine is process-global and the prototype allows only one
  active plug-in instance per host process.
- The Windows build has passed the VST3 SDK validator and command-line audio
  probes, but the plug-in still needs a real DAW installation and live-use test.
  Its bundle contains Sonic Pi source/assets and a trimmed Ruby runtime. The
  package excludes Ruby build tools, headers, documentation, test payloads, and
  caches while retaining the interpreter, libraries, gems, and required DLLs. It also bundles a third-party notice summary and collected upstream license/copyright files.

## Build and run

Requires CMake 3.25+, a C++20 compiler, Rust/Cargo, Git, and network access for
the first configure/build. The build fetches pinned SuperSonic v0.91.0 and
Steinberg VST3 SDK v3.8.1, including their required submodules. Ruby 3.2 must be
available on `PATH` when building and validating the runtime. Windows builds use
the Visual Studio 2022 x64 generator; the build script copies Ruby and the fetched
Sonic Pi source/assets into the VST3 bundle, so an installed plug-in does not use
the build machine's Ruby or source checkout. The bundled Ruby excludes MSYS2
build tools, headers, documentation, test payloads, and package caches while
retaining the interpreter, standard libraries, gems, and required DLLs.

```sh
cmake -S . \
      -B ./build \
      -DCMAKE_BUILD_TYPE=Release
cmake --build ./build \
      --target sonic_pi_vst3 sonic_pi_live_probe sonic_pi_render_probe --parallel 2
./build/sonic_pi_live_probe
mkdir -p generated-artifacts
./build/sonic_pi_render_probe \
  generated-artifacts/sonic-pi-host-buffer-test.wav 2
```

The VST3 bundle is built at
`./build/VST3/Release/sonic_pi_vst3.vst3` on Linux.
The plug-in build runs the VST3 SDK validator. The live probe starts the same
Ruby runtime and embedded engine as the plug-in, runs a Sonic Pi `play` command,
and checks that the host-owned audio buffers contain sound. The render probe
checks variable callback sizes and can write a stereo WAV file.

For a Windows x64 build from a Visual Studio Developer PowerShell:

```powershell
powershell -ExecutionPolicy Bypass `
  -File .\scripts\windows-build.ps1
```

The GitHub Actions workflow builds and validates the Windows package, then uploads
the self-contained `.vst3` bundle as a downloadable artifact. The runtime is stored
at `Contents/Resources/SonicPiVstRuntime`; its `README.txt` lists the bundled
versions, `THIRD_PARTY_NOTICES.txt` summarizes included components, and
`Licenses/FILE_INDEX.txt` maps collected upstream legal notices to their
original locations.

## Licensing note

The wrapper source files use SPDX `GPL-3.0-or-later` identifiers; no separate
copyright holder is claimed for the AI-generated wrapper code. The plug-in
includes SuperSonic v0.91.0, whose combined program is identified upstream as
`AGPL-3.0-or-later`, plus Sonic Pi v5.0.0 files with component-specific terms.
The VST3 SDK is MIT-licensed, while VSTGUI and its third-party components have
their own notices. See `THIRD_PARTY_NOTICES.txt` and the bundled
`Licenses/FILE_INDEX.txt`. The build collects notices from the pinned source
trees and bundled Ruby/MSYS2 runtime, but this is not a legal compliance
certification; review all applicable terms before distribution.
