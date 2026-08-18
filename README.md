# GamePauser

**GamePauser** is a super lightweight utility designed to freeze and resume whitelisted applications instantly. 

### Why use this?
It was built primarily for **language learning in video games**. When dialogue appears and you need extra time to read or look up a word, simply hit the **Pause** key. The software freezes the game's threads, giving you all the time you need without the game world moving on.

> [!TIP]
> **Stability First:** Some games may crash if frozen for too long. GamePauser includes a **Pulse Mode** that alternates between pausing and unpausing at intervals to keep the process "alive" while remaining essentially frozen.

---

## Installation
1. Create a dedicated folder for the app (e.g., `C:\Tools\GamePauser`).
2. Drop `GamePauser.exe` and `whitelist.txt` into that folder.
3. Run `GamePauser.exe`

---

## How to Use
Once running, the app lives in your **System Tray** (near the clock). Right-click the icon to access these features:

* **Add foreground to whitelist:** Automatically adds the currently active window/game to your list.
* **Enable Pulse Mode:** Switches to interval-pausing for games prone to crashing.
* **Open whitelist file:** Opens your `whitelist.txt` for manual editing.
* **Exit:** Fully closes the application and ensures all threads are resumed.

**Default Hotkey:** `Pause/Break` key.
