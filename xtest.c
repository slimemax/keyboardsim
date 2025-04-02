/****************************************************************************
 * xtest_simulator.c
 *
 * An XTest + ncurses program that:
 *  - Has 4 fields: [Text to type], [Start Delay], [Loop Delay], [Loops]
 *  - Supports special tokens: {enter}, {space}, {up}, etc. (with optional :ms hold)
 *  - Loads lines from messages.txt for {messageN}
 *  - F1 => reset fields, F2 => stop typing mid-run
 *  - Logs to an ncurses ring-buffer AND appends to logsXtest.txt
 *
 * Compile:
 *    gcc -o xtest_simulator xtest_simulator.c -lX11 -lXtst -lncurses
 *
 * Run under X11. Press Tab to switch fields, Enter to type, F2 mid-run to stop, 
 * F1 to reset fields, Ctrl+C to quit.
 ****************************************************************************/

#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <X11/extensions/XTest.h>

#include <ncurses.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#define MAX_LOG_LINES 200
static char g_logBuffer[MAX_LOG_LINES][256];
static int  g_logHead = 0;

// We'll keep a file handle for logsXtest.txt
static FILE *g_fileLog = NULL;

/** Global stop flag for F2. When set, we abort mid-typing. */
static int  g_stopRequested = 0;

/** For loading lines from messages.txt -> {messageN}. */
#define MAX_MESSAGES 100
static char *g_messages[MAX_MESSAGES];
static int   g_messageCount = 0;


// ---------------------------------------------------------------------
// add_log
//   Writes to our ring-buffer logs *and* appends to logsXtest.txt
// ---------------------------------------------------------------------
static void add_log(const char *fmt, ...)
{
    // 1) Build the new log string in a temporary buffer
    char tmp[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, args);
    va_end(args);

    // 2) Store in ring buffer
    strncpy(g_logBuffer[g_logHead], tmp, sizeof(g_logBuffer[g_logHead]) - 1);
    g_logBuffer[g_logHead][sizeof(g_logBuffer[g_logHead]) - 1] = '\0';
    g_logHead = (g_logHead + 1) % MAX_LOG_LINES;

    // 3) Also append to logsXtest.txt (if open)
    if (g_fileLog) {
        fprintf(g_fileLog, "%s\n", tmp);
        fflush(g_fileLog);
    }
}

static void draw_logs(int start_line)
{
    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);

    int lines_for_logs = max_y - start_line;
    if (lines_for_logs <= 0) return;

    int index = g_logHead;
    for (int i = 0; i < lines_for_logs; i++) {
        index = (index - 1 + MAX_LOG_LINES) % MAX_LOG_LINES;
        mvprintw(max_y - 1 - i, 0, "%s", g_logBuffer[index]);
    }
}

// ---------------------------------------------------------------------
// Press/Release Keys
// ---------------------------------------------------------------------
static void pressKeyDown(Display *dpy, KeySym ks)
{
    KeyCode kc = XKeysymToKeycode(dpy, ks);
    if (!kc) {
        add_log("WARN: XKeysymToKeycode failed for KeySym=0x%lx (DOWN)", (unsigned long)ks);
        return;
    }
    XTestFakeKeyEvent(dpy, kc, True, CurrentTime);
    XFlush(dpy);
}

static void pressKeyUp(Display *dpy, KeySym ks)
{
    KeyCode kc = XKeysymToKeycode(dpy, ks);
    if (!kc) {
        add_log("WARN: XKeysymToKeycode failed for KeySym=0x%lx (UP)", (unsigned long)ks);
        return;
    }
    XTestFakeKeyEvent(dpy, kc, False, CurrentTime);
    XFlush(dpy);
}

// Quick press+release
static void pressKey(Display *dpy, KeySym ks)
{
    pressKeyDown(dpy, ks);
    usleep(30000);
    pressKeyUp(dpy, ks);
    usleep(30000);
}

// ---------------------------------------------------------------------
// map_char_to_keysym: single normal character -> KeySym
// ---------------------------------------------------------------------
static KeySym map_char_to_keysym(char c)
{
    switch(c)
    {
        case ' ':  return XK_space;
        case '!':  return XK_exclam;
        case '"':  return XK_quotedbl;
        case '#':  return XK_numbersign;
        case '$':  return XK_dollar;
        case '%':  return XK_percent;
        case '&':  return XK_ampersand;
        case '\'': return XK_apostrophe;
        case '(':  return XK_parenleft;
        case ')':  return XK_parenright;
        case '*':  return XK_asterisk;
        case '+':  return XK_plus;
        case ',':  return XK_comma;
        case '-':  return XK_minus;
        case '.':  return XK_period;
        case '/':  return XK_slash;
        case ':':  return XK_colon;
        case ';':  return XK_semicolon;
        case '<':  return XK_less;
        case '=':  return XK_equal;
        case '>':  return XK_greater;
        case '?':  return XK_question;
        case '@':  return XK_at;
        case '[':  return XK_bracketleft;
        case '\\': return XK_backslash;
        case ']':  return XK_bracketright;
        case '^':  return XK_asciicircum;
        case '_':  return XK_underscore;
        case '`':  return XK_grave;
        case '{':  return XK_braceleft;
        case '|':  return XK_bar;
        case '}':  return XK_braceright;
        case '~':  return XK_asciitilde;

        // newline -> Return
        case '\n':
        case '\r':
            return XK_Return;
    }

    // Letters, digits, etc.
    char buf[2] = { c, 0 };
    KeySym ks = XStringToKeysym(buf);
    if (ks == NoSymbol) {
        return NoSymbol;
    }
    return ks;
}

// Quick send of a single normal character
static void send_char(Display *dpy, char c)
{
    KeySym ks = map_char_to_keysym(c);
    if (ks == NoSymbol) {
        add_log("WARN: No KeySym for '%c' (ASCII %d)", c, (int)c);
        return;
    }
    pressKey(dpy, ks);
}

// ---------------------------------------------------------------------
// Loading messages.txt so {messageN} can expand
// ---------------------------------------------------------------------
static void load_messages_file(const char *filename)
{
    FILE *fp = fopen(filename, "r");
    if (!fp) {
        add_log("INFO: Could not open %s, so {messageN} won't work", filename);
        return;
    }

    char linebuf[1024];
    int index = 0;
    while (fgets(linebuf, sizeof(linebuf), fp)) {
        // strip newline
        size_t len = strlen(linebuf);
        while (len > 0 && (linebuf[len-1] == '\n' || linebuf[len-1] == '\r')) {
            linebuf[len-1] = '\0';
            len--;
        }
        g_messages[index] = strdup(linebuf);
        index++;
        if (index >= MAX_MESSAGES) break;
    }
    g_messageCount = index;
    fclose(fp);
    add_log("INFO: Loaded %d lines from %s for {messageN}", g_messageCount, filename);
}

// ---------------------------------------------------------------------
// TokenAction: either a single KeySym press/hold or an "expanded text"
// ---------------------------------------------------------------------
typedef struct {
    KeySym sym;           // 0 if not relevant
    int holdMs;           // 0 if quick press
    char expandBuf[2048]; // if we want to expand text (e.g. from messages.txt)
    int useExpand;        // 1 if expandBuf is valid
} TokenAction;

// ---------------------------------------------------------------------
// parse_special_token: checks if the text at position i is one of:
//   - {up}, {down}, {left}, {right}, {enter}, {shift}, {ctrl}, {alt}, {space}
//   - each can have :NNN hold time, e.g. {up:3000}, {space:1500}
//   - {messageN} => load line #N from messages.txt
// If recognized, returns the length of that token in the string. If not, 0.
// ---------------------------------------------------------------------
static int parse_special_token(const char *text, TokenAction *out)
{
    if (text[0] != '{') return 0;

    // 1) Check if it's {messageN}
    if (strncmp(text, "{message", 8) == 0) {
        int idx = 8;
        char numBuf[32];
        int nb = 0;
        while (text[idx] >= '0' && text[idx] <= '9' && nb < (int)sizeof(numBuf)-1) {
            numBuf[nb++] = text[idx++];
        }
        numBuf[nb] = '\0';

        if (text[idx] == '}') {
            // parse the N
            int msgIndex = atoi(numBuf);
            if (msgIndex < 1 || msgIndex > g_messageCount) {
                add_log("WARN: {message%d} out of range (1..%d)", msgIndex, g_messageCount);
                return 0;
            }
            // valid line
            out->sym       = 0;
            out->holdMs    = 0;
            out->useExpand = 1;
            strncpy(out->expandBuf, g_messages[msgIndex-1], sizeof(out->expandBuf)-1);
            out->expandBuf[sizeof(out->expandBuf)-1] = '\0';

            add_log("SIM: Found token {message%s} => line %d: \"%s\"",
                    numBuf, msgIndex, out->expandBuf);
            return idx + 1; // skip '}'
        }
        return 0;
    }

    // 2) If not {messageN}, check for up/down/left/right/enter/shift/ctrl/alt/space
    struct {
        const char *cmd;
        KeySym sym;
    } table[] = {
        {"up",    XK_Up},
        {"down",  XK_Down},
        {"left",  XK_Left},
        {"right", XK_Right},
        {"enter", XK_Return},
        {"shift", XK_Shift_L},
        {"ctrl",  XK_Control_L},
        {"alt",   XK_Alt_L},
        {"space", XK_space}, // new
        {NULL,    0}
    };

    for (int i = 0; table[i].cmd != NULL; i++) {
        const char *cmd   = table[i].cmd;
        size_t      c_len = strlen(cmd);

        if (strncmp(&text[1], cmd, c_len) == 0) {
            int idx_after = 1 + (int)c_len; // after "up" in "{up"

            if (text[idx_after] == '}') {
                // quick press
                out->sym       = table[i].sym;
                out->holdMs    = 0;
                out->useExpand = 0;
                return idx_after + 1; 
            }
            else if (text[idx_after] == ':') {
                // parse hold time
                int digits_start = idx_after + 1;
                char holdBuf[32];
                int hb = 0;
                while (text[digits_start] >= '0' && text[digits_start] <= '9'
                       && hb < (int)sizeof(holdBuf)-1)
                {
                    holdBuf[hb++] = text[digits_start++];
                }
                holdBuf[hb] = '\0';

                if (text[digits_start] == '}') {
                    out->sym       = table[i].sym;
                    out->holdMs    = atoi(holdBuf);
                    out->useExpand = 0;
                    return digits_start + 1; 
                }
                else {
                    return 0;
                }
            }
            else {
                return 0;
            }
        }
    }

    return 0;
}

// Forward declaration
static void parse_and_type(Display *dpy, const char *text);

// Expand a chunk (like from {messageN}) recursively
static void expand_and_parse(Display *dpy, const char *chunk)
{
    parse_and_type(dpy, chunk);
}

// ---------------------------------------------------------------------
// parse_and_type:
//   Goes through `text`, typing normal chars or handling tokens.
//   We do a small "poll" for F2 after each character/token so
//   the user can press F2 to stop mid-run. 
// ---------------------------------------------------------------------
static void parse_and_type(Display *dpy, const char *text)
{
    int i = 0;

    // Make getch() non-blocking so we can see if user pressed F2 mid-typing
    nodelay(stdscr, TRUE);

    while (text[i] && !g_stopRequested) {
        // Check if user pressed F2 or F1
        int ch = getch();
        while (ch != ERR) {
            if (ch == KEY_F(2)) {
                add_log("F2 pressed => STOP requested");
                g_stopRequested = 1;
            }
            else if (ch == KEY_F(1)) {
                add_log("F1 pressed => resetting fields (in parse_and_type)");
            }
            ch = getch();
        }

        if (g_stopRequested) break;

        // Try to parse a special token
        TokenAction action;
        memset(&action, 0, sizeof(action));

        int consumed = parse_special_token(&text[i], &action);
        if (consumed > 0) {
            if (action.useExpand) {
                // {messageN} => expand text
                add_log("SIM: Insert line => \"%s\"", action.expandBuf);
                expand_and_parse(dpy, action.expandBuf);
            }
            else {
                // a single key press or hold
                if (action.holdMs > 0) {
                    add_log("SIM: Holding KeySym=0x%lx for %d ms", 
                            (unsigned long)action.sym, action.holdMs);
                    pressKeyDown(dpy, action.sym);

                    // Sleep in small increments, so we can see if user hits F2
                    int remain = action.holdMs;
                    const int step = 50; // check every 50 ms
                    while (remain > 0 && !g_stopRequested) {
                        usleep(step * 1000);
                        remain -= step;

                        // Poll again for F2
                        int c2 = getch();
                        while (c2 != ERR) {
                            if (c2 == KEY_F(2)) {
                                add_log("F2 pressed => STOP");
                                g_stopRequested = 1;
                            }
                            else if (c2 == KEY_F(1)) {
                                add_log("F1 pressed => resetting fields (mid hold)");
                            }
                            c2 = getch();
                        }
                        if (g_stopRequested) break;
                    }

                    // done holding
                    pressKeyUp(dpy, action.sym);
                    usleep(30000);
                } else {
                    add_log("SIM: Quick press KeySym=0x%lx", (unsigned long)action.sym);
                    pressKey(dpy, action.sym);
                }
            }
            i += consumed;
        } else {
            // normal single char
            add_log("SIM: Sending char '%c'", text[i]);
            send_char(dpy, text[i]);
            i++;

            // small sleep so the keystrokes aren't instant
            // also to allow user time to press F2
            usleep(30000);
        }
    }

    // Restore blocking getch() in case we continue in the UI
    nodelay(stdscr, FALSE);
}

// ---------------------------------------------------------------------
// simulate_typing: does multiple loops, with start & loop delays
// ---------------------------------------------------------------------
static void simulate_typing(Display *dpy, const char *text, 
                            int loops, int startDelay_ms, int loopDelay_ms)
{
    add_log("SIM: StartDelay=%d, LoopDelay=%d, Loops=%d, text='%s'",
            startDelay_ms, loopDelay_ms, loops, text);

    g_stopRequested = 0; // reset before we begin

    // initial delay
    if (startDelay_ms > 0) {
        add_log("SIM: Sleeping %d ms before typing...", startDelay_ms);
        int remain = startDelay_ms;
        const int step = 50; 
        nodelay(stdscr, TRUE);

        while (remain > 0 && !g_stopRequested) {
            usleep(step * 1000);
            remain -= step;

            // Check for F2
            int c = getch();
            while (c != ERR) {
                if (c == KEY_F(2)) {
                    add_log("F2 pressed => STOP requested (before we start typing)");
                    g_stopRequested = 1;
                } else if (c == KEY_F(1)) {
                    add_log("F1 pressed => resetting fields (pre-typing)");
                }
                c = getch();
            }
        }
        nodelay(stdscr, FALSE);

        if (g_stopRequested) {
            add_log("SIM: Aborted before typing began.");
            return;
        }
    }

    for (int l = 0; l < loops; l++) {
        if (g_stopRequested) break;

        add_log("SIM: Loop %d/%d begin", (l+1), loops);
        parse_and_type(dpy, text);
        if (g_stopRequested) {
            add_log("SIM: Loop interrupted by F2 at loop %d/%d", (l+1), loops);
            break;
        }

        add_log("SIM: Loop %d/%d done", (l+1), loops);
        if (l < loops - 1 && loopDelay_ms > 0) {
            add_log("SIM: Sleeping %d ms before next loop...", loopDelay_ms);

            int remain = loopDelay_ms;
            const int step = 50;
            nodelay(stdscr, TRUE);

            while (remain > 0 && !g_stopRequested) {
                usleep(step * 1000);
                remain -= step;

                int c = getch();
                while (c != ERR) {
                    if (c == KEY_F(2)) {
                        add_log("F2 pressed => STOP requested (between loops)");
                        g_stopRequested = 1;
                    } else if (c == KEY_F(1)) {
                        add_log("F1 pressed => resetting fields (between loops)");
                    }
                    c = getch();
                }
            }
            nodelay(stdscr, FALSE);

            if (g_stopRequested) {
                add_log("SIM: Aborted between loops at loop %d/%d", (l+1), loops);
                break;
            }
        }
    }
    if (!g_stopRequested) {
        add_log("SIM: All loops completed successfully.");
    } else {
        add_log("SIM: Stopped by user (F2).");
    }
}

// ---------------------------------------------------------------------
// main: ncurses UI. F1 => reset fields, F2 => stop. 
// ---------------------------------------------------------------------
int main()
{
    // Open logsXtest.txt in append mode
    g_fileLog = fopen("logsXtest.txt", "a");
    if (!g_fileLog) {
        fprintf(stderr, "WARNING: Could not open logsXtest.txt for append.\n");
        // We'll continue but won't log to file
    }

    // 1) Open X display
    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "ERROR: Could not open X display (not in X11?)\n");
        return 1;
    }

    // 2) Load messages from file
    load_messages_file("messages.txt");

    // 3) Initialize ncurses
    initscr();
    start_color();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);

    init_pair(1, COLOR_CYAN,    COLOR_BLACK);
    init_pair(2, COLOR_GREEN,   COLOR_BLACK);
    init_pair(3, COLOR_YELLOW,  COLOR_BLACK);
    init_pair(4, COLOR_MAGENTA, COLOR_BLACK);
    init_pair(5, COLOR_WHITE,   COLOR_BLACK);

    // Fields
    //   0: text
    //   1: startDelay (ms)
    //   2: loopDelay (ms)
    //   3: loops
    char text[256]         = {0};
    char startDelay_str[16] = "3000";  
    char loopDelay_str[16]  = "2000";  
    char loops_str[16]      = "1";

    int text_pos         = 0;
    int startDelay_pos   = 4; // length("3000")
    int loopDelay_pos    = 4; // length("2000")
    int loops_pos        = 1; // length("1")
    int field            = 0; // active field

    add_log("DEBUG: Program started");
    add_log("TIP: [Tab] to switch fields, [Enter] to type, Ctrl+C to quit.");
    add_log("TIP: F1 => Reset fields, F2 => Stop mid-run.");
    add_log("TIP: e.g. {enter}, {space}, {up:2000}, {message3}, etc.");

    // For aggregated repeated key logging
    static int s_lastKey = -1;
    static int s_repeatCount = 0;

    // Helper to flush repeated key logs
    void flush_key_log() {
        if (s_lastKey >= 0 && s_repeatCount > 0) {
            add_log("DEBUG: Key pressed: %d ('%c') repeated %d time(s)",
                    s_lastKey,
                    (s_lastKey >= 32 && s_lastKey <= 126) ? s_lastKey : '?',
                    s_repeatCount);
        }
        s_lastKey = -1;
        s_repeatCount = 0;
    }

    // Helper to reset fields (called on F1)
    void resetAllFields() {
        text[0] = '\0';       
        text_pos         = 0;
        strcpy(startDelay_str, "3000"); 
        startDelay_pos   = 4;
        strcpy(loopDelay_str,  "2000"); 
        loopDelay_pos    = 4;
        strcpy(loops_str,      "1");    
        loops_pos        = 1;
        add_log("F1: All fields reset to defaults.");
    }

    while (1) {
        erase();

        // Headings
        attron(COLOR_PAIR(1));
        mvprintw(0, 0, "XTest Keyboard Simulator (Ctrl+C to quit)");
        attroff(COLOR_PAIR(1));

        // Field labels
        attron(COLOR_PAIR(2));
        mvprintw(1, 0, "Text to type:");
        attroff(COLOR_PAIR(2));

        attron(COLOR_PAIR(3));
        mvprintw(2, 0, "Start Delay (ms):");
        attroff(COLOR_PAIR(3));

        attron(COLOR_PAIR(4));
        mvprintw(3, 0, "Loop Delay (ms):");
        attroff(COLOR_PAIR(4));

        attron(COLOR_PAIR(5));
        mvprintw(4, 0, "Loops:");
        attroff(COLOR_PAIR(5));

        mvprintw(5, 0, "[Enter => Type, Tab => Switch, F1 => Reset, F2 => Stop]");

        // Show the fields, highlight active
        // "Text to type:" is 13 chars, plus 1 space => column 14 start
        if (field == 0) {
            attron(COLOR_PAIR(2) | A_REVERSE);
            mvprintw(1, 14, "%s", text);
            attroff(COLOR_PAIR(2) | A_REVERSE);
        } else {
            attron(COLOR_PAIR(2));
            mvprintw(1, 14, "%s", text);
            attroff(COLOR_PAIR(2));
        }

        if (field == 1) {
            attron(COLOR_PAIR(3) | A_REVERSE);
            mvprintw(2, 18, "%s", startDelay_str);
            attroff(COLOR_PAIR(3) | A_REVERSE);
        } else {
            attron(COLOR_PAIR(3));
            mvprintw(2, 18, "%s", startDelay_str);
            attroff(COLOR_PAIR(3));
        }

        if (field == 2) {
            attron(COLOR_PAIR(4) | A_REVERSE);
            mvprintw(3, 16, "%s", loopDelay_str);
            attroff(COLOR_PAIR(4) | A_REVERSE);
        } else {
            attron(COLOR_PAIR(4));
            mvprintw(3, 16, "%s", loopDelay_str);
            attroff(COLOR_PAIR(4));
        }

        if (field == 3) {
            attron(COLOR_PAIR(5) | A_REVERSE);
            mvprintw(4, 6, "%s", loops_str);
            attroff(COLOR_PAIR(5) | A_REVERSE);
        } else {
            attron(COLOR_PAIR(5));
            mvprintw(4, 6, "%s", loops_str);
            attroff(COLOR_PAIR(5));
        }

        // Logs
        mvprintw(6, 0, "Logs:");
        draw_logs(7);

        // Put cursor in active field
        if (field == 0) {
            move(1, 14 + text_pos);
        } else if (field == 1) {
            move(2, 18 + startDelay_pos);
        } else if (field == 2) {
            move(3, 16 + loopDelay_pos);
        } else {
            move(4, 6 + loops_pos);
        }

        refresh();

        int ch = getch();

        // Aggregated repeated key logging
        if (ch == s_lastKey) {
            s_repeatCount++;
        } else {
            flush_key_log();
            s_lastKey = ch;
            s_repeatCount = 1;
        }

        // Handle keys
        if (ch == KEY_F(1)) {
            resetAllFields();
        }
        else if (ch == KEY_F(2)) {
            // If currently typing, it sets g_stopRequested
            add_log("F2: Stop requested => Will abort typing if in progress.");
            g_stopRequested = 1;
        }
        else if (ch == '\t') {
            field = (field + 1) % 4;
        }
        else if (ch == '\n') {
            // Convert numeric fields
            int start_ms = atoi(startDelay_str);
            int loop_ms  = atoi(loopDelay_str);
            int loops    = atoi(loops_str);

            if (start_ms < 0) start_ms = 0;
            if (loop_ms < 0)  loop_ms  = 0;
            if (loops < 1)    loops    = 1;

            simulate_typing(dpy, text, loops, start_ms, loop_ms);
        }
        else if (ch == KEY_BACKSPACE || ch == 127) {
            // backspace in active field
            if (field == 0 && text_pos > 0) {
                text[--text_pos] = '\0';
            }
            else if (field == 1 && startDelay_pos > 0) {
                startDelay_str[--startDelay_pos] = '\0';
            }
            else if (field == 2 && loopDelay_pos > 0) {
                loopDelay_str[--loopDelay_pos] = '\0';
            }
            else if (field == 3 && loops_pos > 0) {
                loops_str[--loops_pos] = '\0';
            }
        }
        else if (ch >= ' ' && ch <= '~') {
            // For text, accept all printable chars
            // For numeric fields, digits only
            if (field == 0 && text_pos < (int)(sizeof(text) - 1)) {
                text[text_pos++] = (char)ch;
                text[text_pos] = '\0';
            }
            else if (field == 1 && startDelay_pos < (int)(sizeof(startDelay_str) - 1)
                     && (ch >= '0' && ch <= '9'))
            {
                startDelay_str[startDelay_pos++] = (char)ch;
                startDelay_str[startDelay_pos] = '\0';
            }
            else if (field == 2 && loopDelay_pos < (int)(sizeof(loopDelay_str) - 1)
                     && (ch >= '0' && ch <= '9'))
            {
                loopDelay_str[loopDelay_pos++] = (char)ch;
                loopDelay_str[loopDelay_pos] = '\0';
            }
            else if (field == 3 && loops_pos < (int)(sizeof(loops_str) - 1)
                     && (ch >= '0' && ch <= '9'))
            {
                loops_str[loops_pos++] = (char)ch;
                loops_str[loops_pos] = '\0';
            }
            // else ignore
        }
        // else ignore arrow keys, etc.
    }

    flush_key_log();
    endwin();

    // Cleanup messages
    for (int i = 0; i < g_messageCount; i++) {
        free(g_messages[i]);
        g_messages[i] = NULL;
    }

    // Close the log file if open
    if (g_fileLog) {
        fclose(g_fileLog);
        g_fileLog = NULL;
    }

    XCloseDisplay(dpy);
    return 0;
}

