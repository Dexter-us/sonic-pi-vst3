# Sonic Pi VST3 live-coding prototype

This prototype combines the upstream Sonic Pi Ruby runtime with the
SuperSonic/scsynth engine in a loadable VST3. Its VSTGUI editor contains a
multi-line Sonic Pi code editor and Run/Stop controls. The audio callback
renders directly into the host's stereo output buffers; a separate worker runs
the Sonic Pi language runtime.

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
- The Linux build has been verified with the VST3 SDK validator and command-line
  audio probes, but not in a DAW. A Windows runtime path using Winsock and
  `CreateProcess` has been added; it still needs a native Windows build and DAW
  test.

## Build and run

Requires CMake 3.25+, a C++20 compiler, Rust/Cargo, Git, and network access for
the first configure/build. The build fetches pinned SuperSonic v0.91.0 and
Steinberg VST3 SDK v3.8.1, including their required submodules. Ruby 3.2 must be
available on `PATH` to launch the bundled Sonic Pi runtime. Windows builds use
the Visual Studio 2022 x64 generator and have not yet been verified here.

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

The same build runs through `.github/workflows/windows-build.yml` on pushes,
pull requests, or manual dispatch. The workflow runs the VST3 validator and both
audio probes, then uploads the `.vst3` bundle as a downloadable Actions artifact.

## Licensing note

The plug-in links SuperSonic's GPL-3.0-or-later scsynth engine and Clockwork's
AGPL-3.0-or-later substrate (Clockwork also offers a commercial license). The
Steinberg VST3 SDK is MIT-licensed. Sonic Pi runtime files and synth definitions
carry upstream license terms. Review all applicable licenses before
distributing a combined plug-in; this prototype is not legal advice.
