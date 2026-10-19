# Ruby code generators — v0.1.11

Generators create **Sonic Pi source text**, not sound. They use the bundled
Ruby interpreter in a separate process; they do not share the Sonic Pi runtime.
No extra language installation is needed in the self-contained Windows bundle.

## Workflow

1. Choose **Script** in the editor's top row. A starter generator is provided.
2. Edit it, or use **Open** to load a trusted `.rb` file from
   `Files/Generators` inside the portable `.vst3` folder. **Save** saves Script
   there; Code uses `Files/Code`. Copy external source into those directories
   before opening it. Subdirectories are supported.
3. Choose **Gen/Cancel** or press **Ctrl+Enter** while in Script.
4. Review the trust warning and approve only scripts you trust.
5. The **Preview** view displays the generated source, read-only. Errors are
   also shown there. The original Code buffer remains untouched.
6. Choose **Append** to add the output to the end of Code, or **Replace** to
   replace Code after confirmation.
7. Choose **Run** separately to play the resulting Sonic Pi program.

**Undo gen** restores the previous Code buffer after an append or replacement.
Pressing it again redoes that change. If you manually edit Code afterwards, the
generator undo is disabled so it cannot discard those later edits. Normal editor
undo remains available for ordinary typing.
Switching Code/Script/Preview reloads the editor and resets its local typing
undo history; **Undo gen** is a separate, protected generator-change history.

Click **Gen/Cancel** again to cancel a running generator. Stop/Ctrl+Shift+Enter
still stops Sonic Pi audio; it is separate from cancelling Ruby generation.

## Writing a generator

The script's **final expression must be a Ruby String** containing Sonic Pi
code. Use `puts` or `warn` for diagnostics; these do not become generated source.

```ruby
bpm = 120
notes = [60, 63, 67, 70]
steps = notes.map { |note| "  play #{note}, amp: 0.4\n  sleep 0.25" }

"use_bpm #{bpm}\n\nlive_loop :generated do\n#{steps.join("\n")}\nend\n"
```

Use double-quoted Ruby strings when `\n` should mean a newline. A single-quoted
string preserves `\n` literally.

Ruby's standard library is available. Sonic Pi commands such as `play` are not
generator functions: put them inside the returned String. The generator starts
in a temporary working directory; use explicit paths when reading your own
data files. Temporary runner/output files are cleaned up when it finishes.

## Trust and limits

**This is not a sandbox.** A generator has normal user-account permissions:
it can read or write files, run commands, and use the network. A separate process
keeps execution off the plug-in's UI/audio threads; it does not make untrusted
code safe. Approval is not stored in the DAW project.

- One generator job per plug-in instance.
- Maximum execution time: 5 seconds.
- Maximum generator source: 64 KiB.
- Maximum generated source: 32,000 UTF-8 bytes, without embedded NUL bytes.
- Maximum captured diagnostics: 16 KiB.
- Oversized output, syntax/runtime errors and timeouts do not change Code.
- An empty returned String is a valid result but cannot replace or append code.
- Editing/loading another script or restoring a project invalidates an
  in-progress result. Generate again before applying it.
- Cancel/timeout terminates the worker process group/job. This is cleanup,
  not a security boundary against deliberately escaping child processes.

## Saving a DAW project

Both the Sonic Pi Code buffer and Ruby Script buffer are saved in plug-in state.
Older code-only project states still load, with the default starter generator.
Restoring a project never runs a generator or starts audio automatically.
Previews, running jobs, trust approval and external Save destinations are not
restored. Use Save to choose a file destination again after restoring a project.

## Native regression checks

The offline `Help/editor.html` guide is opened by the editor's Help button.
Snippets are loaded from the portable `Snips` folder and append without
replacing existing Code. Keep the entire `.vst3` folder writable and together
when moving it; its user files and runtime data are included.

The CMake targets `sonic_pi_generator_test`,
`sonic_pi_generator_editor_test` and `sonic_pi_state_test` cover process limits,
cancellation, output validation, explicit apply/undo, unchanged-code guarantees,
backward compatibility and fresh-instance state restoration.
The Windows packaging workflow runs them against the bundled interpreter before
the live-audio probe. Native host window interaction still needs a Windows host
check; these headless tests do not claim to verify clicks or painting.
