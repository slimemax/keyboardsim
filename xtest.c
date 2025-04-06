/****************************************************************************
 * xtest_simulator.c
 *
 * An XTest + ncurses program that:
 *  - Has 4 fields: [Text to type], [Start Delay], [Loop Delay], [Loops]
 *  - Supports special tokens:
 *     {enter}, {space}, {up}, etc. (with optional :ms hold, e.g. {up:3000})
 *     {ctrl+enter} (holds Ctrl while pressing Enter, optional hold time)
 *     {delay:NNN} (delays NNN ms mid-sequence)
 *     {messageN} (loads line N from messages.txt)
 *  - Adds an interactive "countdown" display in the ncurses UI for all delays
 *    or hold times (does NOT spam the ring-buffer logs).
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

/**
 * add_log:
 *   Writes to our ring-buffer logs *and* appends to logsXtest.txt.
 */
static void add_log(const char *fmt, ...)
{
    char tmp[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, args);
    va_end(args);

    strncpy(g_logBuffer[g_logHead], tmp, sizeof(g_logBuffer[g_logHead]) - 1);
    g_logBuffer[g_logHead][sizeof(g_logBuffer[g_logHead]) - 1] = '\0';
    g_logHead = (g_logHead + 1) % MAX_LOG_LINES;

    if (g_fileLog) {
        fprintf(g_fileLog, "%s\n", tmp);
        fflush(g_fileLog);
    }
}

/**
 * draw_logs:
 *   Draw ring-buffer logs on screen starting at 'start_line'.
 */
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

/**
 * pressKeyDown / pressKeyUp / pressKey:
 *   XTest-based helpers to simulate key press/release events.
 */
static void pressKeyDown(Display *dpy, KeySym ks)
{
    KeyCode kc = XKeysymToKeycode(dpy, ks);
    if (!kc) {
        add_log("WARN: XKeysymToKeycode failed for KeySym=0x%lx (DOWN)",
                (unsigned long)ks);
        return;
    }
    XTestFakeKeyEvent(dpy, kc, True, CurrentTime);
    XFlush(dpy);
}

static void pressKeyUp(Display *dpy, KeySym ks)
{
    KeyCode kc = XKeysymToKeycode(dpy, ks);
    if (!kc) {
        add_log("WARN: XKeysymToKeycode failed for KeySym=0x%lx (UP)",
                (unsigned long)ks);
        return;
    }
    XTestFakeKeyEvent(dpy, kc, False, CurrentTime);
    XFlush(dpy);
}

static void pressKey(Display *dpy, KeySym ks)
{
    pressKeyDown(dpy, ks);
    usleep(30000);
    pressKeyUp(dpy, ks);
    usleep(30000);
}

/**
 * map_char_to_keysym:
 *   Convert a single ASCII char to a KeySym (roughly).
 */
static KeySym map_char_to_keysym(char c)
{
    switch(c) {
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
        case '\n':
        case '\r': return XK_Return;
    }
    char buf[2] = { c, 0 };
    KeySym ks = XStringToKeysym(buf);
    if (ks == NoSymbol) {
        return NoSymbol;
    }
    return ks;
}

static void send_char(Display *dpy, char c)
{
    KeySym ks = map_char_to_keysym(c);
    if (ks == NoSymbol) {
        add_log("WARN: No KeySym for '%c' (ASCII %d)", c, (int)c);
        return;
    }
    pressKey(dpy, ks);
}

/**
 * load_messages_file:
 *   Loads lines from messages.txt so {messageN} can expand to line N.
 */
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

/** 
 * countdown_sleep:
 *   Sleeps 'ms' milliseconds in small increments, updating an on-screen 
 *   progress bar or countdown so user can see the time pass. 
 *   Returns 1 if user pressed F2 (stop), else 0 if completed.
 */
static int countdown_sleep(int ms, const char *label)
{
    int remain = ms;
    const int step = 50;  // check about 20x per second
    nodelay(stdscr, TRUE);

    while (remain > 0 && !g_stopRequested) {
        float progress = (float)(ms - remain) / (float)ms;
        int bar_width = 30;
        int fill = (int)(progress * bar_width);

        // We'll display near row 9, col 0 (just below logs, typically).
        mvprintw(9, 0, "%s Delay: [", label);
        for (int i = 0; i < bar_width; i++) {
            if (i < fill) addch('#');
            else addch(' ');
        }
        printw("] %4d ms left  (F2=Stop)", remain);
        clrtoeol();
        refresh();

        usleep(step * 1000);
        remain -= step;

        // Poll for F2
        int ch = getch();
        while (ch != ERR) {
            if (ch == KEY_F(2)) {
                add_log("F2 => STOP");
                g_stopRequested = 1;
            } else if (ch == KEY_F(1)) {
                add_log("F1 => resetting fields (mid-delay)");
            }
            ch = getch();
        }
    }

    // Clear the countdown line
    mvprintw(9, 0, "                                                           ");
    clrtoeol();
    refresh();
    nodelay(stdscr, FALSE);

    return g_stopRequested;
}

/**
 * TokenAction: describes a recognized token like {ctrl+enter}, {delay:NNN}, etc.
 */
typedef struct {
    KeySym sym;      // single key, or 0 if none
    int    holdMs;   // 0 if quick press, else hold time
    char   expandBuf[2048]; // {messageN} expansion
    int    useExpand;        // 1 if expandBuf valid

    int    isCtrlEnter;      // 1 if {ctrl+enter}
    int    isDelay;          // 1 if {delay:NNN}
    int    delayMs;          // used if isDelay=1
} TokenAction;

/**
 * parse_special_token:
 *   Checks if text[i..] is {ctrl+enter}, {delay:NNN}, {messageN}, {up:NNN}, etc.
 *   Returns the length consumed if recognized, else 0.
 */
static int parse_special_token(const char *text, TokenAction *out)
{
    if (text[0] != '{') return 0;

    memset(out, 0, sizeof(*out));

    // {messageN}
    if (strncmp(text, "{message", 8) == 0) {
        int idx = 8;
        char numBuf[32];
        int nb = 0;
        while (text[idx] >= '0' && text[idx] <= '9' && nb < (int)sizeof(numBuf)-1) {
            numBuf[nb++] = text[idx++];
        }
        numBuf[nb] = '\0';

        if (text[idx] == '}') {
            int msgIndex = atoi(numBuf);
            if (msgIndex < 1 || msgIndex > g_messageCount) {
                add_log("WARN: {message%d} out of range (1..%d)",
                        msgIndex, g_messageCount);
                return 0;
            }
            out->useExpand = 1;
            strncpy(out->expandBuf, g_messages[msgIndex-1],
                    sizeof(out->expandBuf)-1);
            out->expandBuf[sizeof(out->expandBuf)-1] = '\0';

            add_log("SIM: Found token {message%s} => line %d: \"%s\"",
                    numBuf, msgIndex, out->expandBuf);
            return idx + 1; // skip '}'
        }
        return 0;
    }

    // {ctrl+enter(:NNN)?}
    if (strncmp(text, "{ctrl+enter", 11) == 0) {
        int idx_after = 11; 
        int hold = 0;
        if (text[idx_after] == ':') {
            idx_after++;
            char holdBuf[32];
            int hb = 0;
            while (text[idx_after] >= '0' && text[idx_after] <= '9'
                   && hb < (int)sizeof(holdBuf)-1)
            {
                holdBuf[hb++] = text[idx_after++];
            }
            holdBuf[hb] = '\0';
            hold = atoi(holdBuf);
        }
        if (text[idx_after] == '}') {
            out->isCtrlEnter = 1;
            out->holdMs      = hold;
            add_log("SIM: Found token {ctrl+enter} holdMs=%d", hold);
            return idx_after + 1; // skip '}'
        }
        return 0;
    }

    // {delay:NNN}
    if (strncmp(text, "{delay:", 7) == 0) {
        int idx_after = 7;
        char delayBuf[32];
        int db = 0;
        while (text[idx_after] >= '0' && text[idx_after] <= '9'
               && db < (int)sizeof(delayBuf)-1)
        {
            delayBuf[db++] = text[idx_after++];
        }
        delayBuf[db] = '\0';
        if (text[idx_after] == '}') {
            out->isDelay = 1;
            out->delayMs = atoi(delayBuf);
            add_log("SIM: Found token {delay:%s} => %d ms", delayBuf, out->delayMs);
            return idx_after + 1; 
        }
        return 0;
    }

    // {up:NNN}, {down}, {space}, etc.
    struct { const char *cmd; KeySym sym; } table[] = {
        {"up",    XK_Up},
        {"down",  XK_Down},
        {"left",  XK_Left},
        {"right", XK_Right},
        {"enter", XK_Return},
        {"shift", XK_Shift_L},
        {"ctrl",  XK_Control_L},
        {"alt",   XK_Alt_L},
        {"space", XK_space},
        {NULL,    0}
    };
    for (int i = 0; table[i].cmd != NULL; i++) {
        const char *cmd = table[i].cmd;
        size_t c_len = strlen(cmd);
        if (strncmp(&text[1], cmd, c_len) == 0) {
            int idx2 = 1 + (int)c_len; 
            if (text[idx2] == '}') {
                out->sym = table[i].sym;
                out->holdMs = 0;
                return idx2 + 1;
            }
            else if (text[idx2] == ':') {
                // parse hold time
                int ds = idx2 + 1;
                char holdBuf[32];
                int hb = 0;
                while (text[ds] >= '0' && text[ds] <= '9'
                       && hb < (int)sizeof(holdBuf)-1)
                {
                    holdBuf[hb++] = text[ds++];
                }
                holdBuf[hb] = '\0';
                if (text[ds] == '}') {
                    out->sym    = table[i].sym;
                    out->holdMs = atoi(holdBuf);
                    return ds + 1;
                }
                return 0;
            }
            return 0;
        }
    }
    return 0;
}

/* Forward declaration */
static void parse_and_type(Display *dpy, const char *text);

/**
 * expand_and_parse:
 *   If {messageN} expands into chunk, we parse that chunk recursively.
 */
static void expand_and_parse(Display *dpy, const char *chunk)
{
    parse_and_type(dpy, chunk);
}

/**
 * parse_and_type:
 *   Process 'text' char by char or token by token, simulating 
 *   key events and supporting F2 to stop mid-run.  
 */
static void parse_and_type(Display *dpy, const char *text)
{
    int i = 0;
    nodelay(stdscr, TRUE);

    while (text[i] && !g_stopRequested) {
        // Check F2 or F1 mid-run
        int ch = getch();
        while (ch != ERR) {
            if (ch == KEY_F(2)) {
                add_log("F2 pressed => STOP requested");
                g_stopRequested = 1;
            } else if (ch == KEY_F(1)) {
                add_log("F1 pressed => resetting fields (in parse_and_type)");
            }
            ch = getch();
        }
        if (g_stopRequested) break;

        // Try special token
        TokenAction action;
        int consumed = parse_special_token(&text[i], &action);
        if (consumed > 0) {
            // {messageN} => expand
            if (action.useExpand) {
                add_log("SIM: Insert line => \"%s\"", action.expandBuf);
                expand_and_parse(dpy, action.expandBuf);
            }
            // {ctrl+enter}
            else if (action.isCtrlEnter) {
                add_log("SIM: Ctrl+Enter (holdMs=%d)", action.holdMs);

                // Press Ctrl
                pressKeyDown(dpy, XK_Control_L);
                usleep(30000);

                // Press + release Enter
                pressKeyDown(dpy, XK_Return);
                usleep(30000);
                pressKeyUp(dpy, XK_Return);
                usleep(30000);

                // If there's a hold period
                if (action.holdMs > 0) {
                    add_log("SIM: Holding Ctrl for %d ms after pressing Enter", action.holdMs);
                    if (!countdown_sleep(action.holdMs, "Ctrl+Enter hold")) {
                        add_log("SIM: Ctrl hold done");
                    }
                }

                // Release Ctrl
                pressKeyUp(dpy, XK_Control_L);
                usleep(30000);
            }
            // {delay:NNN}
            else if (action.isDelay) {
                add_log("SIM: Pausing for %d ms", action.delayMs);
                countdown_sleep(action.delayMs, "Token Delay");
            }
            // Single KeySym w/ optional hold
            else {
                if (action.holdMs > 0) {
                    add_log("SIM: Holding KeySym=0x%lx for %d ms",
                            (unsigned long)action.sym, action.holdMs);
                    pressKeyDown(dpy, action.sym);

                    if (!countdown_sleep(action.holdMs, "Hold Key")) {
                        add_log("SIM: Hold done");
                    }
                    pressKeyUp(dpy, action.sym);
                    usleep(30000);
                } else {
                    add_log("SIM: Quick press KeySym=0x%lx",
                            (unsigned long)action.sym);
                    pressKey(dpy, action.sym);
                }
            }
            i += consumed;
        }
        else {
            // normal char
            add_log("SIM: Sending char '%c'", text[i]);
            send_char(dpy, text[i]);
            i++;
            usleep(30000);
        }
    }

    nodelay(stdscr, FALSE);
}

/**
 * simulate_typing:
 *   Loops the typed text multiple times, with start & loop delays.
 */
static void simulate_typing(Display *dpy, const char *text,
                            int loops, int startDelay_ms, int loopDelay_ms)
{
    add_log("SIM: StartDelay=%d, LoopDelay=%d, Loops=%d, text='%s'",
            startDelay_ms, loopDelay_ms, loops, text);

    g_stopRequested = 0;

    // Start Delay
    if (startDelay_ms > 0 && !g_stopRequested) {
        add_log("SIM: Sleeping %d ms before typing...", startDelay_ms);
        countdown_sleep(startDelay_ms, "Start Delay");
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

        // Loop Delay (if more loops remain)
        if (l < loops - 1 && loopDelay_ms > 0) {
            add_log("SIM: Sleeping %d ms before next loop...", loopDelay_ms);
            countdown_sleep(loopDelay_ms, "Loop Delay");
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

/**
 * main:
 *   Ncurses UI with 4 fields. F1 => reset, F2 => stop.
 */
int main()
{
    g_fileLog = fopen("logsXtest.txt", "a");
    if (!g_fileLog) {
        fprintf(stderr, "WARNING: Could not open logsXtest.txt for append.\n");
    }

    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "ERROR: Could not open X display (not in X11?)\n");
        return 1;
    }

    load_messages_file("messages.txt");

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

    char text[256]          = {0};
    char startDelay_str[16] = "3000";
    char loopDelay_str[16]  = "2000";
    char loops_str[16]      = "1";

    int text_pos         = 0;
    int startDelay_pos   = 4; // length("3000")
    int loopDelay_pos    = 4; // length("2000")
    int loops_pos        = 1; // length("1")
    int field            = 0; // which field is active

    add_log("DEBUG: Program started");
    add_log("TIP: [Tab] to switch fields, [Enter] to type, F1=Reset, F2=Stop, Ctrl+C=Quit.");
    add_log("TIP: Tokens => {enter}, {space}, {ctrl+enter:1000}, {delay:3000}, {message3}, etc.");

    static int s_lastKey = -1;
    static int s_repeatCount = 0;

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

        attron(COLOR_PAIR(1));
        mvprintw(0, 0, "XTest Keyboard Simulator (Ctrl+C to quit)");
        attroff(COLOR_PAIR(1));

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

        mvprintw(5, 0, "[Enter=>Type  Tab=>Switch  F1=>Reset  F2=>Stop]");

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

        mvprintw(6, 0, "Logs:");
        draw_logs(7);

        // Place cursor
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

        if (ch == s_lastKey) {
            s_repeatCount++;
        } else {
            flush_key_log();
            s_lastKey = ch;
            s_repeatCount = 1;
        }

        if (ch == KEY_F(1)) {
            resetAllFields();
        }
        else if (ch == KEY_F(2)) {
            add_log("F2 => Stop requested => Will abort typing if in progress.");
            g_stopRequested = 1;
        }
        else if (ch == '\t') {
            field = (field + 1) % 4;
        }
        else if (ch == '\n') {
            int start_ms = atoi(startDelay_str);
            int loop_ms  = atoi(loopDelay_str);
            int loops    = atoi(loops_str);

            if (start_ms < 0) start_ms = 0;
            if (loop_ms < 0)  loop_ms  = 0;
            if (loops < 1)    loops    = 1;

            simulate_typing(dpy, text, loops, start_ms, loop_ms);
        }
        else if (ch == KEY_BACKSPACE || ch == 127) {
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
            // Text => accept all printable chars
            // Numeric => digits only
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
        }
        // else ignore arrow keys, etc.
    }

    flush_key_log();
    endwin();

    for (int i = 0; i < g_messageCount; i++) {
        free(g_messages[i]);
        g_messages[i] = NULL;
    }

    if (g_fileLog) {
        fclose(g_fileLog);
        g_fileLog = NULL;
    }
    XCloseDisplay(dpy);
    return 0;
}

