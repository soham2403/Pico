#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#define CTRL_KEY(k) ((k) & 0x1f)

enum editorKey {
    ARROW_LEFT = 1000,
    ARROW_RIGHT,
    ARROW_UP,
    ARROW_DOWN,
    BACKSPACE = 127
};

struct editorConfig {
    int cx, cy;
    int rowoff;
    int screen_rows, screen_cols;
    int numrows;
    char **rows;
    char *filename;
    struct termios orig_termios;
};

static struct editorConfig E;

/* ---------- Utils ---------- */

ssize_t safe_write(int fd, const void *buf, size_t count) {
    ssize_t n;
    while ((n = write(fd, buf, count)) == -1 && errno == EINTR)
        ;
    return n;
}

void die(const char *msg) {
    safe_write(STDOUT_FILENO, "\x1b[2J\x1b[H", 7);
    perror(msg);
    exit(1);
}

/* ---------- Terminal ---------- */

void disableRawMode(void) {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios);
}

void enableRawMode(void) {
    if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1)
        die("tcgetattr");

    atexit(disableRawMode);

    struct termios raw = E.orig_termios;
    raw.c_iflag &= ~(ICRNL | IXON);
    raw.c_lflag &= ~(ECHO | ICANON | ISIG);
    raw.c_oflag &= ~(OPOST);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1)
        die("tcsetattr");
}

int editorReadKey(void) {
    char c;
    while (read(STDIN_FILENO, &c, 1) != 1)
        ;

    if (c == '\x1b') {
        char seq[2];
        if (read(STDIN_FILENO, &seq[0], 1) != 1)
            return '\x1b';
        if (read(STDIN_FILENO, &seq[1], 1) != 1)
            return '\x1b';

        if (seq[0] == '[') {
            switch (seq[1]) {
            case 'A':
                return ARROW_UP;
            case 'B':
                return ARROW_DOWN;
            case 'C':
                return ARROW_RIGHT;
            case 'D':
                return ARROW_LEFT;
            }
        }
        return '\x1b';
    }
    return c;
}

void getWindowSize(void) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1)
        die("ioctl");

    E.screen_rows = ws.ws_row - 1;
    E.screen_cols = ws.ws_col;
}

/* ---------- File ---------- */

void editorOpen(const char *filename) {
    FILE *fp = fopen(filename, "r");
    E.filename = strdup(filename);

    if (!fp)
        return;

    char *line = NULL;
    size_t cap = 0;
    ssize_t len;

    while ((len = getline(&line, &cap, fp)) != -1) {
        line[strcspn(line, "\n")] = '\0';
        E.rows = realloc(E.rows, sizeof(char *) * (E.numrows + 1));
        E.rows[E.numrows++] = strdup(line);
    }

    free(line);
    fclose(fp);
}

void editorSave(void) {
    int fd = open(E.filename, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd == -1)
        return;

    for (int i = 0; i < E.numrows; i++) {
        safe_write(fd, E.rows[i], strlen(E.rows[i]));
        safe_write(fd, "\n", 1);
    }
    close(fd);
}

/* ---------- Editing ---------- */

void ensureRowExists(int y) {
    while (y >= E.numrows) {
        E.rows = realloc(E.rows, sizeof(char *) * (E.numrows + 1));
        E.rows[E.numrows++] = strdup("");
    }
}

void editorInsertChar(int c) {
    ensureRowExists(E.cy);

    char *row = E.rows[E.cy];
    int len = strlen(row);

    row = realloc(row, len + 2);
    memmove(&row[E.cx + 1], &row[E.cx], len - E.cx + 1);
    row[E.cx] = c;
    E.rows[E.cy] = row;
    E.cx++;
}

void editorDelChar(void) {
    if (E.cy >= E.numrows || E.cx == 0)
        return;

    char *row = E.rows[E.cy];
    int len = strlen(row);

    memmove(&row[E.cx - 1], &row[E.cx], len - E.cx + 1);
    E.cx--;
}

void editorInsertNewline(void) {
    ensureRowExists(E.cy);

    char *row = E.rows[E.cy];

    E.rows = realloc(E.rows, sizeof(char *) * (E.numrows + 1));
    memmove(&E.rows[E.cy + 2], &E.rows[E.cy + 1],
            sizeof(char *) * (E.numrows - E.cy - 1));

    E.rows[E.cy + 1] = strdup(&row[E.cx]);
    row[E.cx] = '\0';
    E.rows[E.cy] = realloc(row, strlen(row) + 1);

    E.numrows++;
    E.cy++;
    E.cx = 0;
}

/* ---------- Drawing ---------- */

void editorDrawRows(void) {
    for (int y = 0; y < E.screen_rows; y++) {
        int filerow = y + E.rowoff;
        if (filerow < E.numrows) {
            safe_write(STDOUT_FILENO, E.rows[filerow], strlen(E.rows[filerow]));
        }
        safe_write(STDOUT_FILENO, "\x1b[K\r\n", 5);
    }
}

void editorDrawStatusBar(void) {
    char status[80];
    int len = snprintf(status, sizeof(status),
                       " %s | Ctrl-S Save | Ctrl-Q Quit ", E.filename);

    safe_write(STDOUT_FILENO, "\x1b[7m", 4);
    safe_write(STDOUT_FILENO, status, len);
    while (len < E.screen_cols) {
        safe_write(STDOUT_FILENO, " ", 1);
        len++;
    }
    safe_write(STDOUT_FILENO, "\x1b[0m", 4);
}

void editorRefreshScreen(void) {
    safe_write(STDOUT_FILENO, "\x1b[?25l\x1b[H", 10);
    editorDrawRows();
    editorDrawStatusBar();

    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.cy - E.rowoff) + 1, E.cx + 1);
    safe_write(STDOUT_FILENO, buf, strlen(buf));
    safe_write(STDOUT_FILENO, "\x1b[?25h", 6);
}

/* ---------- Input ---------- */

void editorMoveCursor(int key) {
    ensureRowExists(E.cy);

    int rowlen = strlen(E.rows[E.cy]);

    switch (key) {
    case ARROW_LEFT:
        if (E.cx > 0)
            E.cx--;
        break;
    case ARROW_RIGHT:
        if (E.cx < rowlen)
            E.cx++;
        break;
    case ARROW_UP:
        if (E.cy > 0)
            E.cy--;
        break;
    case ARROW_DOWN:
        if (E.cy + 1 < E.numrows)
            E.cy++;
        break;
    }

    rowlen = strlen(E.rows[E.cy]);
    if (E.cx > rowlen)
        E.cx = rowlen;

    if (E.cy < E.rowoff)
        E.rowoff = E.cy;
    if (E.cy >= E.rowoff + E.screen_rows)
        E.rowoff = E.cy - E.screen_rows + 1;
}

void editorProcessKeypress(void) {
    int c = editorReadKey();

    switch (c) {
    case CTRL_KEY('q'):
        safe_write(STDOUT_FILENO, "\x1b[2J\x1b[H", 7);
        exit(0);
    case CTRL_KEY('s'):
        editorSave();
        break;
    case '\r':
        editorInsertNewline();
        break;
    case BACKSPACE:
        editorDelChar();
        break;
    case ARROW_UP:
    case ARROW_DOWN:
    case ARROW_LEFT:
    case ARROW_RIGHT:
        editorMoveCursor(c);
        break;
    default:
        if (isprint(c))
            editorInsertChar(c);
        break;
    }
}

/* ---------- Main ---------- */

void initEditor(void) {
    E.cx = E.cy = 0;
    E.rowoff = 0;
    E.numrows = 0;
    E.rows = NULL;
    getWindowSize();
}

int main(int argc, char *argv[]) {

    if (argc != 2) {
        fprintf(stderr, "Usage: pico <file>\n");
        exit(1);
    }

    enableRawMode();
    initEditor();
    editorOpen(argv[1]);

    while (1) {
        editorRefreshScreen();
        editorProcessKeypress();
    }
}
