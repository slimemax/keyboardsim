Below is a suggested **README** that documents what the program does, how to build and run it, and shows many example usage scenarios (including special tokens, timers, arrows, etc.). Following that, you’ll see a short primer on how to commit the `xtest_simulator.c` file in your `xtest` folder to GitHub.

---

# XTest Keyboard Simulator

**Filename**: `xtest_simulator.c`  
**Purpose**: A command-line / ncurses-based utility for automating keyboard input under X11 (Linux), featuring:

- Ability to type text strings (including normal characters and special tokens).
- Delays before and between loops of typing.
- Support for multiple loops of the same text.
- Loading lines from `messages.txt` for dynamic insertion via `{messageN}` tokens.
- On-screen status logs (via an ncurses ring buffer).
- Persistent logging to `logsXtest.txt`.
- Real-time interruption (via F2) to stop an ongoing typing sequence.

## Overview of Features

1. **Typed Fields**  
   The ncurses interface provides four editable fields:
   - **Text to Type**: The main text or token sequence you want to simulate typing.
   - **Start Delay (ms)**: A delay in milliseconds before typing begins (once you press Enter).
   - **Loop Delay (ms)**: A delay in milliseconds between repeated loops of typing.
   - **Loops**: The number of times to repeat typing the text.

2. **Special Tokens** (within the “Text to Type” field)
   - **Arrow Keys**: `{up}`, `{down}`, `{left}`, `{right}`
     - Example: `{up}`, `{down:3000}` (hold the down arrow for 3000ms)
   - **Enter**: `{enter}` (or you can manually include `\n` in your text)
   - **Space**: `{space}`
   - **Shift**: `{shift}`, with optional hold time: `{shift:1000}`
   - **Ctrl**: `{ctrl}`, with optional hold time: `{ctrl:250}`
   - **Alt**: `{alt}`, with optional hold time: `{alt:500}`
   - **Message Expansion**: `{messageN}` (expands to the Nth line of `messages.txt`)
     - Example: `{message1}`, `{message2}`, etc.

   If you add a `:ms` suffix to a token—like `{down:2000}`—it means “press and hold” that key for 2000 milliseconds. Without `:ms`, the press is a quick press+release.

3. **Loading from `messages.txt`**  
   - If you include tokens like `{message3}`, the program will look up the 3rd line of `messages.txt` and expand that token into the content of that line.
   - If `messages.txt` cannot be opened, you’ll see a log message indicating `{messageN}` expansions will not work.

4. **Controls and Hotkeys**  
   - **Tab**: Cycles through the four fields (`Text to type`, `Start Delay`, `Loop Delay`, `Loops`).
   - **Enter**: Begins the typing simulation using the current fields.
   - **F1**: Resets all fields to their defaults (`Text to type` is cleared, other fields revert to `3000`, `2000`, and `1`).
   - **F2**: Stops/aborts typing mid-run. If you press F2 while text is being typed, the run halts immediately.
   - **Ctrl + C**: Quits the program altogether.

5. **Logging**  
   - A ring buffer of logs is shown at the bottom of the ncurses window.
   - All logs also get appended to `logsXtest.txt`.
   - The ring buffer can hold up to 200 lines, after which it overwrites the oldest logs.

6. **Compile and Run**
   - **Compile**:  
     ```bash
     gcc -o xtest_simulator xtest_simulator.c -lX11 -lXtst -lncurses
     ```
   - **Run**:  
     ```bash
     ./xtest_simulator
     ```
   - Must be run under X11 (i.e., you need a valid `$DISPLAY`).

## How to Use

1. **Start the Program**  
   ```bash
   ./xtest_simulator
   ```
   The ncurses UI will appear with four fields and a log area.

2. **Edit Fields**  
   - Move between fields using **Tab**.
   - Type into the active field (the highlighted one).
     - “Text to type” accepts **any** printable character, plus embedded tokens (like `{enter}`, `{up:2000}`, etc.).
     - “Start Delay (ms)”, “Loop Delay (ms)”, and “Loops” accept only digit characters (`0-9`).

3. **Trigger the Typing**  
   - Once your fields are set, press **Enter** to begin typing automation.
   - The program will wait for the “Start Delay” in milliseconds, then type out the “Text to type.” If you have multiple loops, it repeats, waiting “Loop Delay” ms between loops.

4. **Stop or Reset**  
   - To abort mid-typing, press **F2**. 
   - To reset the fields at any time, press **F1**.

## Example Scenarios

### 1. Simple text typing
- **Fields**:
  - Text to type: `Hello world!`
  - Start Delay (ms): `1000`
  - Loop Delay (ms): `2000`
  - Loops: `2`
- **Behavior**:
  - Wait 1 second, type `Hello world!`, wait 2 seconds, type `Hello world!` again, then stop.

### 2. Using `{enter}` to type multi-line content
- **Fields**:
  - Text to type: `First line{enter}Second line`
  - Start Delay (ms): `3000`
  - Loop Delay (ms): `1000`
  - Loops: `1`
- **Behavior**:
  - After 3 seconds, types `First line`, presses **Enter**, then types `Second line`, finishing in one loop.

### 3. Holding arrow keys
- **Fields**:
  - Text to type: `{down:2000}Now we go down{enter}{up:3000}Now up`
  - Start Delay (ms): `2000`
  - Loop Delay (ms): `1000`
  - Loops: `1`
- **Behavior**:
  - After 2 seconds:
    - Holds the **Down Arrow** for 2 seconds.
    - Types `Now we go down`.
    - Presses **Enter**.
    - Holds the **Up Arrow** for 3 seconds.
    - Types `Now up`.

### 4. Using `messages.txt` expansions
Assume `messages.txt` has these lines:
```
Line 1: Lorem ipsum
Line 2: Dolor sit amet
Line 3: {enter}This contains an enter token
```
- **Fields**:
  - Text to type: `Begin{enter}{message1}{enter}{message3}`
  - Start Delay (ms): `1000`
  - Loop Delay (ms): `1000`
  - Loops: `2`
- **Behavior**:
  - Waits 1 second, types `Begin`, presses Enter, then expands `{message1}` → `Lorem ipsum`, presses Enter, then expands `{message3}` → (which itself has `\{enter\}` inside!).  
  - Repeats a second time (because Loops=2).

### 5. Combining everything
- **Fields**:
  - Text to type: `Go left{left:1500}{space}Shift test{shift:1000}Done`
  - Start Delay (ms): `2000`
  - Loop Delay (ms): `3000`
  - Loops: `3`
- **Behavior**:
  1. Wait 2 seconds.
  2. Types “Go left”.
  3. Holds **Left Arrow** for 1.5 seconds.
  4. Types a **Space**.
  5. Types “Shift test”.
  6. Holds **Shift** for 1 second (no visible effect in text, but it’s part of the simulation).
  7. Types “Done”.
  8. Waits 3 seconds, repeats for 3 total loops.

## Program Flow Summary

1. **Launch**: Program starts in ncurses mode, initializes logs, reads in lines from `messages.txt`.
2. **UI**: You see 4 fields plus a rolling log area.  
3. **Filling Fields**:  
   - **Text to type** can contain normal characters and special tokens.  
   - **Start Delay** (ms), **Loop Delay** (ms), and **Loops** are numeric.  
4. **Press Enter** to begin the automated typing:
   - The program waits “Start Delay” ms (checking if F2 is pressed to abort).
   - It types out the “Text to type,” expanding any tokens along the way (again, each token or character can be interrupted if F2 is pressed).
   - If “Loops” > 1, it waits “Loop Delay” ms, then types again, until loops are complete or F2 aborts.
5. **Log Output**: 
   - The bottom log area shows each step, including warnings for unknown tokens or out-of-range `{messageN}` references.
   - Everything is appended to `logsXtest.txt`.

## Building the Program

1. Make sure you have the development libraries installed for X11, Xtst, and ncurses. For example, on Ubuntu/Debian:
   ```bash
   sudo apt-get update
   sudo apt-get install libx11-dev libxtst-dev libncurses-dev
   ```
2. Compile with:
   ```bash
   gcc -o xtest_simulator xtest_simulator.c -lX11 -lXtst -lncurses
   ```

3. Run:
   ```bash
   ./xtest_simulator
   ```

4. (Optional) **messages.txt** in the same directory is used to expand `{messageN}` tokens.

## Known Limitations

- Must be run under X11 (not Wayland unless you have XWayland and the correct environment).
- Only prints out standard ASCII keysyms for normal characters; some extended or special characters may not map directly.
- The ring-buffer log can overwrite older log lines if you run it for a very long time.

