A fuller menu bar and a Settings menu. AmiFM works as before; the start drawers and the programs it hands files to can now be set from inside it.

**What's new**
- **Menus** (right mouse button): AmiFM gains **About...** (name, version, licence); Selection gains **Copy**, **Move** and **Add Icon**; the main actions have Amiga-key shortcuts (the README has the full table).
- **Settings > Start drawers...**: the drawers the left and right panes open on (default `SYS:` and `RAM:`). **Current panes** fills in what the panes show now. A drawer that isn't there at start (volume not mounted) falls back to the default.
- **Settings > Programs...**: what View, Edit, Extract and Pack run (defaults `SYS:Utilities/MultiView`, `ENV:EDITOR` or `SYS:Tools/TextEdit`, `xadUnFile`, `lha`).
- Each page has **Save** (`ENV:` and `ENVARC:AmiFM.prefs`), **Use** (`ENV:` only, until reboot), **Defaults** and **Cancel**, and fits a 640x200 screen.
- **Settings > Snapshot window** saves the window's place and size; **Unsnapshot window** goes back to the default. Both change only the `WINDOW` line of `AmiFM.prefs`, so settings you only Used stay Used.
- Without a snapshot the window now opens at most 720 pixels wide, so on a wide or two-monitor Workbench it fits one monitor.
- If the window can't be reopened after Iconify, AmiFM no longer goes on reading the closed window's messages.
- AmiFM is GPL-3.0; the `LICENSE` file is in the drawer.

Use the **.lha**: it keeps the executable bit, which a .zip can't store (see the README).
