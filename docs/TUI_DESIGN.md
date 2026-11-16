# Leopard Installer 1.6.0 — TUI design

Status: implemented in `1.6.0_1`. The shipped `1.5.1` CLI/PTY installer stays
available as the proven fallback.

## Goal

Replace the line-oriented interactive path with a full-screen TUI while
keeping:

- the declarative InstallPlan and the ordered step sequence;
- `auto=1` zero-input installs;
- serial logging for headless tests;
- the existing install/format/package profile files;
- the ability to run a step program independently.

The TUI is built on `lpcurses` (MIT) and therefore on `leopard-conhost`'s PTY/VT:

```text
installer TUI (lpcurses)
  -> ANSI/VT on fd 1
  -> conhost
  -> framebuffer
```

## Non-goals

- No mouse.
- No graphical installer.
- No terminfo database.
- No changes to the package-selection or partition data model.
- No curses initialization in `auto=1` or other non-tty runs.

## Modes

| Mode | Trigger | UI |
|---|---|---|
| `auto` | `auto=1` | none; existing serial/PTY log path |
| `line` | non-tty, `LEOPARD_INSTALL_TUI=0`, or lpcurses init failure | existing prompts |
| `tui` | interactive tty and lpcurses available | full-screen installer |

`main.c` selects the mode once. A failed `initscr()` falls back to `line`.

## Architecture

Steps remain separate programs and exchange data only through
`/config/installer/state`. They never call `initscr()`.

The TUI lives in the orchestrator; steps talk to it through inherited pipes:

```text
installer main (TUI owner)
  +-- lpcurses screen
  +-- step sequencer
  +-- LI_UI_FD (host -> step) / LI_UI_OUT_FD (step -> host)
  +-- step stdout/stderr pipe -> log pane
  +-- state file <--> step child
```

Files:

```text
include/li_ui.h + lib/li_ui.c        step-side protocol client, line/auto fallback
include/li_tui.h + lib/li_tui.c      lpcurses frame, menus, input, confirm, log ring
include/li_ui_host.h + lib/li_ui_host.c
                                     orchestrator: pipes, fork/exec, request routing
```

If the UI pipes are absent, a step uses the existing line/auto behavior.

## Environment

The orchestrator sets these for each step child:

```text
LI_UI_MODE=tui
LI_UI_FD=<fd>       # host -> step
LI_UI_OUT_FD=<fd>   # step -> host
LI_STATE_DIR=/config/installer
```

The step's stdin is closed; stdout/stderr go to the log pipe. Steps never touch
the real terminal. This also prevents an unconverted prompt from stealing TUI
keystrokes.

## UI protocol

Newline-delimited ASCII. Fields are TAB-separated; the last field may contain
spaces. Titles/prompts are sanitized so they cannot inject separators.

### Step -> orchestrator

```text
STEP <title>
MENU <request-id>\t<title>\t<selected-index>
ITEM <index>\t<title>\t<description>
END
INPUT <request-id>\t<prompt>\t<default>
CONFIRM <request-id>\t<prompt>\t<y|n>
PROGRESS <0..100>\t<text>
LOG <text>
DONE <rc>
ERROR <text>
```

`MENU`/`INPUT`/`CONFIRM` are requests: the step blocks reading the reply pipe
until the matching answer arrives.

### Orchestrator -> step

```text
ANSWER <request-id>\t<value>
CANCEL <request-id>
```

`CANCEL` maps to Esc/back. Progress and log events are asynchronous; they may
arrive while a request is pending, so the orchestrator drains the log pipe from
inside modal key loops.

## `li_ui` API

```c
int  li_ui_mode(void);                 /* LI_UI_TUI / LI_UI_LINE / LI_UI_AUTO */
int  li_ui_menu(const char *title, const li_ui_item *items, int count,
                int selected);
int  li_ui_input(const char *prompt, char *out, size_t cap);
int  li_ui_confirm(const char *prompt, int default_yes);
void li_ui_step(const char *title);
void li_ui_progress(int percent, const char *text);
void li_ui_log(const char *fmt, ...);
void li_ui_done(int rc);
```

Behavior:

- `tui`: speak the pipe protocol; never call `xapi_Input` or print a prompt.
- `line`: call the existing `xapi_Input`/`xapi_Printf` path.
- `auto`: return defaults immediately; no blocking.

`li_core.c` also routes the historical `li_ask` / `li_confirm` through the TUI
when `LI_UI_FD` is present, so unconverted steps still get modal input instead
of reading the terminal.

## Screen layout

Minimum 80x24; larger terminals get more step/log rows.

```text
+ LeopardOS Installer ------------------------------------------+
| Step 3/11  Select packages                          3/11     |
| [############----------------------------------]  27%        |
| Running xbps-install ...                                     |
+--------------------------------------------------------------+
| [x] Disk       [x] Plan        [x] Partition                 |
| [x] Format     [>] Mount       [ ] Packages select           |
| [ ] Packages   [ ] Config      [ ] Bootloader                |
| [ ] Verify     [ ] Finish                                    |
+--------------------------------------------------------------+
| log                                                          |
| == Mount target filesystems ==                               |
| [step] Mount target filesystems                              |
| mounted /dev/sata0p1 on /mnt/boot                            |
+--------------------------------------------------------------+
| Enter Select  Esc Back  F2 Log                               |
+--------------------------------------------------------------+
```

Keys:

- Up/Down or `j`/`k`: move
- Home/End/PgUp/PgDn: jump
- Enter: select/confirm
- Esc: back/cancel
- F2: toggle the log pane
- Ctrl+C: no special handler; conhost/kernel policy applies

Destructive operations (disk erase, write partition table) always require an
explicit `Yes`; the default is `No` for disk erase and `Yes` for the resolved
partition table.

## Orchestrator loop

1. parse the sequence into an array of step paths;
2. choose `auto`/`line`/`tui`;
3. in TUI mode call `li_ui_host_init()` and render the welcome menu;
4. for each step:
   - create the UI pipes and the log pipe;
   - fork/exec with `LI_UI_FD` / `LI_UI_OUT_FD`;
   - poll step pipes, the UI pipe, and the child's exit;
   - route menu/input/confirm requests to lpcurses;
   - update the step list, progress bar, and log ring;
5. on failure show Retry / Shut down;
6. on success show Reboot / Shut down;
7. `endwin()` on every exit path.

`auto`/`line` modes keep the `fork`/`execve` + `waitpid` + serial mirror path.

## Log capture

The child's stdout/stderr are a pipe, not the PTY. The host:

- polls the log fd before reading (**never read first**; an empty pipe would
  block the modal loop and keyboard input would appear dead);
- accumulates partial lines;
- appends complete lines to a 256-line ring;
- renders the bottom pane (last N lines).

`li_log` still mirrors to the serial port, so headless tests keep their
`== Step ==` / `[step] result=ok` assertions.

Modal boundaries are also serial markers:

```text
installer: ui welcome
installer: ui menu <title>
installer: ui input <prompt>
installer: ui confirm <prompt>
installer: ui error <step>
installer: ui finish
```

## Error handling

- Step failure: modal with exit code, current step, log pane, Retry / Shut down.
- Missing package: existing core/optional fallback remains; TUI shows a warning,
  not a blocking error unless core packages failed.
- Pipe EOF before `DONE`: step failure; exit code is authoritative.
- lpcurses init failure: fall back to line mode and continue.
- Resize: `KEY_RESIZE` redraws; the install is not aborted.
- Terminal restore: `endwin()` on every normal/TUI exit path.

## PTY / session teardown

Two issues showed up while validating the TUI and the installed system:

1. **Installer log pump**: the host must `poll()` the step log pipe before
   `read()`. Reading an empty pipe inside a modal blocks the key loop, so the
   TUI looks dead even though the PTY still has input. `host_read_log()` polls
   first; this is the only TUI-side PTY-related fix.

2. **conhost term_fd**: conhost keeps a lazily opened slave fd for
   `pty_no_echo()` (IME password context). After the session child exits, that
   extra slave reference kept the PTY pair alive, so the master read never
   reported EOF and conhost waited for `master_eof` forever. getty was not
   restarted after OOBE. `leopard-conhost 1.0_6` closes the cached `term_fd`
   as soon as `waitpid()` reports the child (or `ECHILD`), before waiting for
   EOF.

The kernel `vfs_close()` PTY path must keep the original per-close callback
semantics: `slave_fds`/`master_fds` are handle counts, decremented on every
close. Deferring the callback to `node->refcount == 0` looks correct but the
slave node carries a creation reference held by `pair->slave_node`, so the
count never reaches zero and EOF is never signalled. An experimental kernel
change along those lines was reverted; the kernel release string stays
`2.2.1_1` (new build, not a version bump).

## Build and packaging

- `leopard-installer 1.6.0` links `liblpcurses.a` statically.
- `lpcurses` is build-time only; no runtime package dependency.
- Live PE still contains one `leopard-installer.elf` plus step ELFs.
- `auto` live media keeps the current serial assertions.
- The TUI test image is the same ISO without `auto=1`.

## Test plan

1. `leopard-os/tests/test_install.sh` with `auto=1`: unchanged serial assertions.
2. `leopard-os/tests/test_install_tui.sh`: QEMU HMP `sendkey`, headless (no
   display needed because the VT is rendered by conhost):
   welcome -> disk -> erase confirm -> profile -> partition confirm ->
   package profile -> package confirm -> install -> finish.
3. Boot test after the TUI install.
4. `LEOPARD_INSTALL_TUI=0` fallback.
5. Serial log contains step banners and `installer: all steps complete.`
