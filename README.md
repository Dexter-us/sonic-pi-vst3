# Dexter U.S. Sonic Pi VST3 live-coding instrument

**v0.1.12:** Snippets loads `.rb` / `.txt` files from the portable `Snips` folder
and appends without replacing selections. Source files use `Files/Code` or
`Files/Generators`; runtime data and logs use `RuntimeData`. These directories
and the offline `Help/editor.html` guide live inside the `.vst3` folder.
The Windows packaging check executes every shipped snippet and creates
`Help/snippet-checks.html` with the exact tested source and audio results.
Keep the entire bundle in a writable location and back up user files before upgrades.

**v0.1.11:** trusted Ruby code generators now have separate Script and read-only
Preview views, explicit Append/Replace and generator-edit undo. Both source
buffers are saved with the DAW project; generation never starts playback.
See [GENERATORS.md](GENERATORS.md) for the workflow, limits and trust warning.

This prototype combines the upstream Sonic Pi Ruby runtime with the
SuperSonic/scsynth engine in a loadable VST3. Its VSTGUI editor contains a
multi-line Sonic Pi code editor with Open/Save, syntax highlighting, Sonic Pi
completion choices, reusable code snippets, buffer/selection execution, a
`live_loop` picker, runtime diagnostics, code help, and Run/Stop controls.
Ctrl+Space opens completions, Ctrl+Enter runs the buffer, Ctrl+Alt+Enter runs
the selected code, Ctrl+Shift+Enter stops all jobs, and Ctrl+Shift+Space opens
code help. The Loops button lists complete `live_loop` blocks found in the
current buffer. The log panel shows recent Spider runtime output and errors;
reported error lines are highlighted, and Go to error navigates to the line
when it can be matched to the current buffer. Select a Sonic Pi name and choose
Docs to show its description and example. The audio callback renders directly
into the host's stereo output buffers;
a separate worker runs the Sonic Pi language runtime. It is registered as an
Instrument/Synth by Dexter U.S., and the editor header carries the same branding.

## DAW project state

Version 0.1.10 embeds the current editor buffer in VST3 component and controller
state, including code that has not been saved to an external `.rb` file. Saving
and reopening a DAW project restores that code even if the editor was closed.
Closing and reopening the plug-in editor also keeps the buffer.
Restoration does **not** execute the code: press Run when ready.

External absolute file paths are deliberately not stored in a project. Restored
code appears as an Untitled buffer, and Save asks where to write a file. This
keeps portable projects from accidentally overwriting a file on another machine.
The host-state format is versioned, preserves UTF-8 and empty buffers, and rejects
invalid/truncated state without replacing the existing code. The maximum saved
buffer is 1 MiB; this does not change the runtime's smaller Run-message limit.
This saves source code, not the Ruby heap, running jobs, or a live audio snapshot.

Headless state regression check (after configuring CMake):

```sh
cmake --build build --target sonic_pi_state_test --parallel 2
# Pass the generated VST3 bundle path to the sonic_pi_state_test executable.
```

## Scope and current limitations

- The plug-in does not read DAW tempo or transport. Sonic Pi gets a free-running
  internal clock, initially 60 BPM, so its own scheduler can run independently
  of the host.
- There is no VST MIDI input or output path. MIDI-related Sonic Pi APIs are not
  bridged by this prototype.
- The embedded runtime does not expose Sonic Pi's standalone GUI recording
  command. Record the plug-in's audio using the DAW's normal recording tools.
- Completion and snippet choices insert at the text cursor, replacing any
  selected text. They are built into the plug-in and do not require Vim,
  Neovim, or an external Sonic Pi CLI.
- Sonic Pi's file-based buffer loads are decoded and copied into Clockwork
  buffers by the OSC adapter. The `supersonic-audio-out` device synth is not
  used; audio is delivered through the host's output buffers.
- The embedded engine is process-global and the prototype allows only one
  active plug-in instance per host process.
- The Linux build has been verified with the VST3 SDK validator and command-line
  audio probes, but not in a DAW. The Windows package includes the Sonic Pi
  source/assets and a trimmed Ruby runtime inside the VST3 bundle. It excludes
  Ruby build tools, headers, documentation, tests, and caches while retaining
  runtime libraries and DLLs. It also bundles a third-party notice summary and
  collected upstream license/copyright files; it still needs a real DAW
  installation and live-use test.

## Build and run

Requires CMake 3.25+, a C++20 compiler, Rust/Cargo, Git, and network access for
the first configure/build. The build fetches pinned SuperSonic v0.91.0 and
Steinberg VST3 SDK v3.8.1, including their required submodules. Ruby 3.2 must be
available on `PATH` when building and validating the runtime. Windows builds use
the Visual Studio 2022 x64 generator; the build script copies Ruby and the
fetched Sonic Pi source/assets into the VST3 bundle, so the installed plug-in
does not use the build machine's Ruby or source checkout.

```sh
cmake -S native/sonic-pi-callback-spike \
      -B native/sonic-pi-callback-spike/build \
      -DCMAKE_BUILD_TYPE=Release
cmake --build native/sonic-pi-callback-spike/build \
      --target sonic_pi_vst3 sonic_pi_live_probe sonic_pi_render_probe --parallel 2
native/sonic-pi-callback-spike/build/sonic_pi_live_probe
mkdir -p generated-artifacts
native/sonic-pi-callback-spike/build/sonic_pi_render_probe \
  generated-artifacts/sonic-pi-host-buffer-test.wav 2
```

The VST3 bundle is built at
`native/sonic-pi-callback-spike/build/VST3/Release/sonic_pi_vst3.vst3` on Linux.
The plug-in build runs the VST3 SDK validator. The live probe starts the same
Ruby runtime and embedded engine as the plug-in, runs a Sonic Pi `play` command,
and checks that the host-owned audio buffers contain sound. The render probe
checks variable callback sizes and can write a stereo WAV file.

For a Windows x64 build from a Visual Studio Developer PowerShell:

```powershell
powershell -ExecutionPolicy Bypass `
  -File .\native\sonic-pi-callback-spike\scripts\windows-build.ps1
```

The same build runs through `.github/workflows/windows-build.yml` on pushes,
pull requests, or manual dispatch. The workflow runs the VST3 validator and both
audio probes against the packaged runtime, then uploads the self-contained
`.vst3` bundle as a downloadable Actions artifact. On Windows the runtime is at
`Contents/Resources/SonicPiVstRuntime`; its `README.txt` records the bundled
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
