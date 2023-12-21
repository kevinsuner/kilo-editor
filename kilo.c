/*** includes ***/

#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/*** defines ***/

#define KILO_VERSION "0.0.1"
#define KILO_TAB_STOP 8
#define KILO_QUIT_TIMES 3

// bitwise-ANDs a character with the value 00011111, in binary. (In C, you generally specify
// bitmasks using hexadecimal, since C doesn't have binary literals, and hexadecimal is more
// concise and readable.) In other words, it sets the upper 3 bits of the character to 0. This
// mirrors what the Ctrl key does in the terminal: it strips bits 5 and 6 from whatever key you
// press in combination with Ctrl, and sends that
#define CTRL_KEY(k) ((k) & 0x1f)

enum editorKey {
	BACKSPACE = 127,
	ARROW_LEFT = 1000,
	ARROW_RIGHT,
	ARROW_UP,
	ARROW_DOWN,
	DEL_KEY,
	HOME_KEY,
	END_KEY,
	PAGE_UP,
	PAGE_DOWN
};

/*** data ***/

typedef struct erow {
	int size;
	int rsize;
	char *chars;
	char *render;
} erow;

struct editorConfig {
	int cx, cy;
	int rx;
	int rowoff;
	int coloff;
	int screenrows;
	int screencols;
	int numrows;
	erow *row;
	int dirty;
	char *filename;
	char statusmsg[80];
	time_t statusmsg_time;
	struct termios og_termios;
};

struct editorConfig E;

/*** prototypes ***/

void editorSetStatusMessage(const char *fmt, ...);
void editorRefreshScreen();
char *editorPrompt(char *prompt);

/*** terminal ***/

void die(const char *s) {
	write(STDOUT_FILENO, "\x1b[2J", 4);
	write(STDOUT_FILENO, "\x1b[H", 3);

	perror(s);
	exit(1);
}

void disableRawMode() {
	// Sets the terminal attributes for the file descriptor STDIN_FILENO, and the optional_actions
	// to TCSAFLUSH from the termios structure referenced by *termios_p
	if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.og_termios) == -1)
		die("tcsetattr");
}

void enableRawMode() {
	// Gets the terminal attributes for the file descriptor STDIN_FILENO from the termios structure
	// referenced by *termios_p
	if (tcgetattr(STDIN_FILENO, &E.og_termios) == -1) die("tcgetattr");
	atexit(disableRawMode);

	struct termios raw = E.og_termios;
	
	// IXON comes from <termios.h>. The I stands for "input flag" and XON comes from the names of the
	// two control characters that Ctrl-S and Ctrl-Q product: XOFF to pause transmission and XON to
	// resume transmission.
	// BRKINT will cause a SIGINT signal to be sent to the program, like pressing Ctr-C.
	// INPCK enables parity checking.
	// ISTRIP causes the 8th bit of each input byte to be stripped, meaning it will set it to 0
	raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);

	// OPOST comes from <termios.h>. O means it's an output flag, and I assume POST stands for post-
	// processimg of output
	raw.c_oflag &= ~(OPOST);

	// CS8 is not a flag, it is a bit mask with multiple bits, which we set using the bitwise-OR (|)
	// operator, unlike all the flags we are turning off. It sets the character size (CS) ro 8 bits
	// per byte
	raw.c_cflag |= (CS8);

	// ECHO is a bitflag defined as 00000000000000000000000000001000 in binary. Here we use bitwise-NOT
	// operator (~) on this value to get 11111111111111111111111111110111. Then we use bitwise-AND which
	// forces the fourth bit in the flags field to become 0, and causes every other bit to retain its
	// current value.
	// ICANON comes from <termios.h>. Input flags (the ones in the c_iflag field) generally start with I
	// like ICANON does. However ICANON is not an input flag, it's a "local" flag in the c_lflag field.
	// Confusing indeed
	raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);

	// The VMIN value sets the minimum number of bytes of input needed before read() can return. We set
	// it to 0 so that read() returns as there is any input to be read.
	// c_cc field stands for "control characters", an array of bytes that control various terminal settings
	raw.c_cc[VMIN] = 0;

	// The VTIME value sets the maximum amount of time to wait before read() returns. It is in tenths of
	// a second, so we set it to 1/10 of a second, or 100 milliseconds. If read() times out, it will return
	// 0, which makes sense because it's usual return value is the number of bytes read
	raw.c_cc[VTIME] = 1;

	if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) die("tcsetattr");
}

int editorReadKey() {
	int nread;
	char c;
	while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
		if (nread == -1 && errno != EAGAIN) die("read");
	}
	
	if (c == '\x1b') {
		char seq[3];

		if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
		if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';

		if (seq[0] == '[') {
			if (seq[1] >= '0' && seq[1] <= '9') {
				if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';
				if (seq[2] == '~') {
					switch (seq[1]) {
						case '1': return HOME_KEY;
						case '3': return DEL_KEY;
						case '4': return END_KEY;
						case '5': return PAGE_UP;
						case '6': return PAGE_DOWN;
						case '7': return HOME_KEY;
						case '8': return END_KEY;
					}
				}
			} else {
				switch (seq[1]) {
					case 'A': return ARROW_UP;
					case 'B': return ARROW_DOWN;
					case 'C': return ARROW_RIGHT;
					case 'D': return ARROW_LEFT;
					case 'H': return HOME_KEY;
					case 'F': return END_KEY;
				}
			}
		} else if (seq[0] == 'O') {
			switch (seq[1]) {
				case 'H': return HOME_KEY;
				case 'F': return END_KEY;
			}
		}

		return '\x1b';
	} else {
		return c;
	}
}

int getCursorPosition(int *rows, int *cols) {
	char buf[32];
	unsigned int i = 0;

	// The n command [Device Status Report](https://v100.net/docs/vt100-ug/chapter3.html/#DSR) can be used
	// to query the terminal for status information. We want to give it an argument 6 to ask for the cursor
	// position
	if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) return -1;

	while (i < sizeof(buf) - 1) {
		if (read(STDIN_FILENO, &buf[i], 1) != 1) break;
		if (buf[i] == 'R') break;
		i++;
	}
	buf[i] = '\0';

	// First we make sure it responded with an escape sequence
	if (buf[0] != '\x1b' || buf[1] != '[') return -1;

	// Then we pass a pointer to the third character of buf to sscanf(), skipping the '\x1b' and '[' characters.
	// So we are passing a string of the form 24;80 to sscanf(). We are also passing it the string &d;%d which
	// tells it to parse two integers separated by a ;, and put the values into the rows and cols variables
	if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) return -1;

	return 0;
}

int getWindowSize(int *rows, int *cols) {
	struct winsize ws;

	// On success, ioctl() will place the number of columns wide and the number of rows high the terminal
	// is into the given winsize struct. On failure ioctl() returns -1. We also check to make sure the
	// values it gave back weren't 0, because apparently that's a possible erroneous outcome.
	// TIOCGWINSZ apparently stands for "Terminal Input/Output Control Get Window Size"
	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
		// Fallback for when ioctl() isn't an option. We are sending two escape sequences one after the other.
		// The C command [Cursor Forward](https://vt100.net/docs/vt100-ug/chapter3.html#CUF) moves the cursor to the right,
		// and the B command [Cursor Down](https://vt100.net/docs/vt100-ug/chapter3.html#CUD) moves the cursor down.
		// The argument says how much to move it right or down by. We use a very large value 999, which should ensure
		// that the cursor reaches the right and bottom edges of the screen. The C and B commands are specifically documented
		// to stop the cursor from going past the edge of the screen
		if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) return -1;
		return getCursorPosition(rows, cols);
	} else {
		*cols = ws.ws_col;
		*rows = ws.ws_row;
		return 0;
	}
}

/*** row operations ***/

int editorRowCxToRx(erow *row, int cx) {
	int rx = 0;
	int j;
	for (j = 0; j < cx; j++) {
		if (row->chars[j] == '\t')
			rx += (KILO_TAB_STOP - 1) - (rx % KILO_TAB_STOP);
		rx++;
	}
	return rx;
}

void editorUpdateRow(erow *row) {
	int tabs = 0;
	int j;
	for (j = 0; j < row->size; j++)
		if (row->chars[j] == '\t') tabs++;

	free(row->render);
	row->render = malloc(row->size + tabs*(KILO_TAB_STOP - 1) + 1);

	int idx = 0;
	for (j = 0; j < row->size; j++) {
		if (row->chars[j] == '\t') {
			row->render[idx++] = ' ';
			while (idx % KILO_TAB_STOP != 0) row->render[idx++] = ' ';
		} else {
			row->render[idx++] = row->chars[j];
		}
	}
	row->render[idx] = '\0';
	row->rsize = idx;
}

void editorInsertRow(int at, char *s, size_t len) {
	if (at < 0 || at > E.numrows) return;

	E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));
	memmove(&E.row[at + 1], &E.row[at], sizeof(erow) * (E.numrows - at));

	E.row[at].size = len;
	E.row[at].chars = malloc(len + 1);
	memcpy(E.row[at].chars, s, len);
	E.row[at].chars[len] = '\0';

	E.row[at].rsize = 0;
	E.row[at].render = NULL;
	editorUpdateRow(&E.row[at]);

	E.numrows++;
	E.dirty++;
}

void editorFreeRow(erow *row) {
	free(row->render);
	free(row->chars);
}

void editorDelRow(int at) {
	if (at < 0 || at > E.numrows) return;
	editorFreeRow(&E.row[at]);
	memmove(&E.row[at], &E.row[at + 1], sizeof(erow) * (E.numrows - at - 1));
	E.numrows--;
	E.dirty++;
}

void editorRowInsertChar(erow *row, int at, int c) {
	if (at < 0 || at > row->size) at = row->size;
	row->chars = realloc(row->chars, row->size + 2);
	memmove(&row->chars[at + 1], &row->chars[at], row->size - at + 1);
	row->size++;
	row->chars[at] = c;
	editorUpdateRow(row);
	E.dirty++;
}

void editorRowAppendString(erow *row, char *s, size_t len) {
	row->chars = realloc(row->chars, row->size + len + 1);
	memcpy(&row->chars[row->size], s, len);
	row->size += len;
	row->chars[row->size] = '\0';
	editorUpdateRow(row);
	E.dirty++;
}

void editorRowDelChar(erow *row, int at) {
	if (at < 0 || at >= row->size) return;
	memmove(&row->chars[at], &row->chars[at+1], row->size - at);
	row->size--;
	editorUpdateRow(row);
	E.dirty++;
}

/*** editor operations ***/

void editorInsertChar(int c) {
	if (E.cy == E.numrows) {
		editorInsertRow(E.numrows, "", 0);
	}
	editorRowInsertChar(&E.row[E.cy], E.cx, c);
	E.cx++;
}

void editorInsertNewline() {
	if (E.cx == 0) {
		editorInsertRow(E.cy, "", 0);
	} else {
		erow *row = &E.row[E.cy];
		editorInsertRow(E.cy + 1, &row->chars[E.cx], row->size - E.cx);
		row = &E.row[E.cy];
		row->size = E.cx;
		row->chars[row->size] = '\0';
		editorUpdateRow(row);
	}
	E.cy++;
	E.cx = 0;
}

void editorDelChar() {
	if (E.cy == E.numrows) return;
	if (E.cx == 0 && E.cy == 0) return;

	erow *row = &E.row[E.cy];
	if (E.cx > 0) {
		editorRowDelChar(row, E.cx - 1);
		E.cx--;
	} else {
		E.cx = E.row[E.cy - 1].size;
		editorRowAppendString(&E.row[E.cy - 1], row->chars, row->size);
		editorDelRow(E.cy);
		E.cy--;
	}
}

/*** file i/o ***/

char *editorRowsToString(int *buflen) {
	int totlen = 0;
	int j;
	for (j = 0; j < E.numrows; j++) {
		totlen += E.row[j].size + 1;
	}
	*buflen = totlen;

	char *buf = malloc(totlen);
	char *p = buf;
	for (j = 0; j < E.numrows; j++) {
		memcpy(p, E.row[j].chars, E.row[j].size);
		p += E.row[j].size;
		*p = '\n';
		p++;
	}

	return buf;
}

void editorOpen(char *filename) {
	free(E.filename);
	E.filename = strdup(filename);

	FILE *fp = fopen(filename, "r");
	if (!fp) die("fopen");

	char *line = NULL;
	size_t linecap = 0;
	ssize_t linelen;

	// getline() is useful for reading lines from a file when we don't know how much memory to allocate
	// for each line. It takes care of memory management for you. First, we pass it a null line pointer
	// and a linecap (line capacity) of 0. That makes it allocate new memory for the next line it reads,
	// and set line to point to the memory, and set linecap to let you know how much memory is allocated.
	// It's return value is the length of the line it read, or -1 if it's at the end of the file and there
	// are no more lines to read
	while ((linelen = getline(&line, &linecap, fp)) != -1) {
		// We also strip off the newline or carriage return at the end of the line before copying it into
		// our errow. We know each erow represents one line of text, so there's no use storing a newline
		// character at the end of each one
		while (linelen > 0 && (line[linelen - 1] == '\n' ||
								line[linelen - 1] == '\r'))
			linelen--;
		editorInsertRow(E.numrows, line, linelen);
	}

	free(line);
	fclose(fp);
	E.dirty = 0;
}

void editorSave() {
	if (E.filename == NULL) {
		E.filename = editorPrompt("Save as: %s (ESC to cancel)");
		if (E.filename == NULL) {
			editorSetStatusMessage("Save aborted");
			return;
		}
	}

	int len;
	char *buf = editorRowsToString(&len);

	int fd = open(E.filename, O_RDWR | O_CREAT, 0644);
	if (fd != -1) {
		// ftruncate() sets the file's size to the specified length. If the file is larger than that, it will
		// cut off any data at the end of the file to make it that length. If the file is shorter, it will add
		// 0 bytes at the end to make it that length.
		// The normal way to overwrite a file is to pass the O_TRUNC flag to open(), which truncates the file
		// completely, making it an empty file, before writing the new data into it. By truncating the file
		// ourselves to the same length as the data we are planning to write into it, we are making the whole
		// overwriting operation a little bit safer in case the ftruncate() call succeeds but the write()
		// fails. In that case, the file would still contain most of the data it had before. But if the file
		// was truncated completely by the open() call and then the write() failed, you'd end up with all your data lost
		if (ftruncate(fd, len) != -1) {
			if (write(fd, buf, len) == len) {
				close(fd);
				free(buf);
				E.dirty = 0;
				editorSetStatusMessage("%d bytes written to disk", len);
				return;
			}
			close(fd);
		}
	}

	free(buf);
	editorSetStatusMessage("Can't save! I/O error: %s", strerror(errno));
}

/*** append buffer ***/

struct abuf {
	char *b;
	int len;
};

#define ABUF_INIT {NULL, 0}

void abAppend(struct abuf *ab, const char *s, int len) {
	// To append a string s to an abuf, the first thing we do is make sure we allocate enough memory to hold
	// the new string. We ask realloc() to give us a block of memory that is the size of the current string
	// plus the size of the string we are appending. realloc() will either extend the size of the block of
	// memory that we already have allocated, or it will take care of free()ing the current block of memory
	// and allocating a new block of memory somewhere else that is big enough for our new string
	char *new = realloc(ab->b, ab->len + len);

	if (new == NULL) return;

	// We use memcpy to copy the string s after the end of the current data in the buffer
	memcpy(&new[ab->len], s, len);
	
	// Then we update the pointer and length of the abuf to the new values
	ab->b = new;
	ab->len += len;
}

void abFree(struct abuf *ab) {
	free(ab->b);
}

/*** output ***/

void editorScroll() {
	E.rx = 0;
	if (E.cy < E.numrows) {
		E.rx = editorRowCxToRx(&E.row[E.cy], E.cx);
	}

	if (E.cy < E.rowoff) {
		E.rowoff = E.cy;
	}
	if (E.cy >= E.rowoff + E.screenrows) {
		E.rowoff = E.cy - E.screenrows + 1;
	}
	if (E.rx < E.coloff) {
		E.coloff = E.rx;
	}
	if (E.rx >= E.coloff + E.screencols) {
		E.coloff = E.rx - E.screencols + 1;
	}
}

void editorDrawRows(struct abuf *ab) {
	int y;
	for (y = 0; y < E.screenrows; y++) {
		int filerow = y + E.rowoff;
		if (filerow >= E.numrows) {
			if (E.numrows == 0 && y == E.screenrows / 3) {
				char welcome[80];
				int welcomelen = snprintf(welcome, sizeof(welcome), "Kilo editor -- version %s", KILO_VERSION);
				if (welcomelen > E.screencols) welcomelen = E.screencols;

				// To center a string, you divide the screen width by 2, and then subtract half of the string's length
				// from that. In other words: E.screencols/2 - welcomelen/2, which simplifies to (E.screencols - welcomelen) / 2
				// That tells you how far from the left edge of the screen you should start printing the string.
				// So we fill that space with space characters, except for the first character, which should be a tilde
				int padding = (E.screencols - welcomelen) / 2;
				if (padding) {
					abAppend(ab, "~", 1);
					padding--;
				}
				while (padding--) abAppend(ab, " ", 1);
				abAppend(ab, welcome, welcomelen);
			} else {
				abAppend(ab, "~", 1);
			}
		} else {
			int len = E.row[filerow].rsize - E.coloff;
			if (len < 0) len = 0;
			if (len > E.screencols) len = E.screencols;
			abAppend(ab, &E.row[filerow].render[E.coloff], len);
		}

		// The K command [Erase In Line](https://vt100.net/docs/vt100-ug/chapter3.html#EL)
		// erases part of the current line. Its argument is analogous to the J command's argument: 2 erases the whole line,
		// 1 erases the part of the line to the left of the cursor, and 0 erases the part of the line to the right of the
		// cursor. 0 is the default argument, and that's what we want
		abAppend(ab, "\x1b[K", 3);
		abAppend(ab, "\r\n", 2);
	}
}

void editorDrawStatusBar(struct abuf *ab) {
	// The m command [Select Graphic Rendition](https://vt100.net/docs/vt100-ug/chapter3.html#SGR) causes the text printed
	// after it to be printed with various possible attributes including bold (1), underscore (4), blink (5), and inverted
	// colors (7). For example, you could specify all of these attributes using the command <esc>[1;4;5;7m. An argument of 0
	// clears all attributes, and is the default argument, so we use <esc>[m to go back to normal text formatting
	abAppend(ab, "\x1b[7m", 4);
	char status[80], rstatus[80];
	int len = snprintf(status, sizeof(status), "%.20s - %d lines %s", 
		E.filename ? E.filename : "[No Name]", E.numrows,
		E.dirty ? "(modified)" : "");
	int rlen = snprintf(rstatus, sizeof(rstatus), "%d/%d", E.cy + 1, E.numrows);
	if (len > E.screencols) len = E.screencols;
	abAppend(ab, status, len);
	while (len < E.screencols) {
		if (E.screencols - len == rlen) {
			abAppend(ab, rstatus, rlen);
			break;
		} else {
			abAppend(ab, " ", 1);
			len++;
		}
	}
	abAppend(ab, "\x1b[m", 3);
	abAppend(ab, "\r\n", 2);
}

void editorDrawMessageBar(struct abuf *ab) {
	abAppend(ab, "\x1b[K", 3);
	int msglen = strlen(E.statusmsg);
	if (msglen > E.screencols) msglen = E.screencols;
	if (msglen && time(NULL) - E.statusmsg_time < 5) {
		abAppend(ab, E.statusmsg, msglen);
	}
}

void editorRefreshScreen() {
	editorScroll();

	struct abuf ab = ABUF_INIT;

	// The first byte is \x1b which is the escape character, or 27 in decimal.
	// This escape sequence is 6 bytes long, and uses the l command [Reset Mode](https://vt100.net/docs/vt100-ug/chapter3.html#RM)
	// to reset one or more terminal modes as specified by each selective parameter in the parameter string
	abAppend(&ab, "\x1b[?25l", 6);

	// This escape sequence is only 3 bytes long, and uses the H command [Cursor Position](https://vt100.net/docs/vt100-ug/chapter3.html#CUP)
	// to position the cursor. The H command actually takes two arguments: the row number and the column number
	// at which to position the cursor. So if you have an 80x24 size terminal and you want the cursor in the 
	// center of the screen, you could use the command <esc>[12;40H. (Multiple arguments are separated by a ; character.)
	// The default argument for H both happen to be 1, so we can leave both arguments out and it will
	// position the cursor at the first row and first column.
	abAppend(&ab, "\x1b[H", 3);

	editorDrawRows(&ab);
	editorDrawStatusBar(&ab);
	editorDrawMessageBar(&ab);

	char buf[32];
	snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.cy - E.rowoff) + 1, 
												(E.rx - E.coloff) + 1);
	abAppend(&ab, buf, strlen(buf));

	// This escape sequence is 6 bytes long, and uses the h command [Set Mode](https://vt100.net/docs/vt100-ug/chapter3.html#SM)
	// and causes one or more modes to be set within the terminal as specified by each selective parameter in the parameter string
	abAppend(&ab, "\x1b[?25h", 6);

	write(STDOUT_FILENO, ab.b, ab.len);
	abFree(&ab);
}

// The ... argument makes editorSetStatusMessage() a [Variadic function](https://en.wikipedia.org/wiki/variadic_function)
// meaning that it can take any number of arguments. C's way of dealing with these arguments is by having you call va_start()
// and va_end() on a value of type va_list. The last argument before the ... (in this case, fmt) must be passed to va_start()
// so that the address of the next argument is known. Then, between the va_start() and va_end() calls, you would call va_arg()
// and pass it the type of the next argument (which you usually get from the given format string) and it would return the value
// of that argument. In this case, we pass fmt and ap to vsnprintf() and it takes care of reading the format string and calling
// va_arg() to get each argument
void editorSetStatusMessage(const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);

	// vsnprintf() helps us make our own printf()-style function
	vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
	
	va_end(ap);

	// We set E.statusmsg_time to the current time which can be gotten by passing NULL to time().
	// It returns the number of seconds that have passed since [midnight, January 1, 1970](https://en.wikipedia.org/wiki/Unix_time)
	// as an integer
	E.statusmsg_time = time(NULL);
}

/*** input ***/

char *editorPrompt(char *prompt) {
	size_t bufsize = 128;
	char *buf = malloc(bufsize);

	size_t buflen = 0;
	buf[0] = '\0';

	while (1) {
		editorSetStatusMessage(prompt, buf);
		editorRefreshScreen();

		int c = editorReadKey();
		if (c == DEL_KEY || c == CTRL_KEY('h') || c == BACKSPACE) {
			if (buflen != 0) buf[--buflen] = '\0'; 
		} else if (c == '\x1b') {
			editorSetStatusMessage("");
			free(buf);
			return NULL;
		} else if (c == '\r') {
			if (buflen != 0) {
				editorSetStatusMessage("");
				return buf;
			}
		} else if (!iscntrl(c) && c < 128) {
			if (buflen == bufsize - 1) {
				bufsize *= 2;
				buf = realloc(buf, bufsize);
			}
			buf[buflen++] = c;
			buf[buflen] = '\0';
		}
	}
}

void editorMoveCursor(int key) {
	erow *row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];

	switch (key) {
		case ARROW_LEFT:
			if (E.cx != 0) {
				E.cx--;
			} else if (E.cy > 0) {
				E.cy--;
				E.cx = E.row[E.cy].size;
			}
			break;
		case ARROW_RIGHT:
			if (row && E.cx < row->size) {
				E.cx++;
			} else if (row && E.cx == row->size) {
				E.cy++;
				E.cx = 0;
			}
			break;
		case ARROW_UP:
			if (E.cy != 0) {
				E.cy--;
			}
			break;
		case ARROW_DOWN:
			if (E.cy < E.numrows) {
				E.cy++;
			}
			break;
	}

	row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];
	int rowlen = row ? row->size : 0;
	if (E.cx > rowlen) {
		E.cx = rowlen;
	}
}

void editorProcessKeypress() {
	static int quit_times = KILO_QUIT_TIMES;

	int c = editorReadKey();

	switch (c) {
		case '\r':
			editorInsertNewline();
			break;

		case CTRL_KEY('q'):
			if (E.dirty && quit_times > 0) {
				editorSetStatusMessage("WARNING!!! File has unsaved changes. "
					"Press Ctrl-Q %d more times to quit.", quit_times);
				quit_times--;
				return;
			}
			write(STDOUT_FILENO, "\x1b[2J", 4);
			write(STDOUT_FILENO, "\x1b[H", 3);
			exit(0);
			break;

		case CTRL_KEY('s'):
			editorSave();
			break;

		case HOME_KEY:
			E.cx = 0;
			break;

		case END_KEY:
			if (E.cy < E.numrows)
				E.cx = E.row[E.cy].size;
			break;

		case BACKSPACE:
		case CTRL_KEY('h'):
		case DEL_KEY:
			if (c == DEL_KEY) editorMoveCursor(ARROW_RIGHT);
			editorDelChar();
			break;
		
		case PAGE_UP:
		case PAGE_DOWN:
			{
				if (c == PAGE_UP) {
					E.cy = E.rowoff;
				} else if (c == PAGE_DOWN) {
					E.cy = E.rowoff + E.screenrows - 1;
					if (E.cy > E.numrows) E.cy = E.numrows;
				}

				int times = E.screenrows;
				while (times--)
					editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
			}
			break;

		case ARROW_UP:
		case ARROW_DOWN:
		case ARROW_LEFT:
		case ARROW_RIGHT:
			editorMoveCursor(c);
			break;

		case CTRL_KEY('l'):
		case '\x1b':
			break;

		default:
			editorInsertChar(c);
			break;
	}

	quit_times = KILO_QUIT_TIMES;
}

/*** init ***/

void initEditor() {
	E.cx = 0;
	E.cy = 0;
	E.rx = 0;
	E.rowoff = 0;
	E.coloff = 0;
	E.numrows = 0;
	E.row = NULL;
	E.dirty = 0;
	E.filename = NULL;
	E.statusmsg[0] = '\0';
	E.statusmsg_time = 0;

	if (getWindowSize(&E.screenrows, &E.screencols) == -1) die("getWindowSize");
	E.screenrows -= 2;
}

int main(int argc, char *argv[]) {
	enableRawMode();
	initEditor();
	if (argc >= 2) {
		editorOpen(argv[1]);
	}

	editorSetStatusMessage("HELP: Ctrl-S = save | Ctrl-Q = quit");

	while (1) {
		editorRefreshScreen();
		editorProcessKeypress();
	}

	return 0;
}
