/*** includes ***/

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

/*** defines ***/

#define KILO_VERSION "0.0.1"

// bitwise-ANDs a character with the value 00011111, in binary. (In C, you generally specify
// bitmasks using hexadecimal, since C doesn't have binary literals, and hexadecimal is more
// concise and readable.) In other words, it sets the upper 3 bits of the character to 0. This
// mirrors what the Ctrl key does in the terminal: it strips bits 5 and 6 from whatever key you
// press in combination with Ctrl, and sends that
#define CTRL_KEY(k) ((k) & 0x1f)

enum editorKey {
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

struct editorConfig {
	int cx, cy;
	int screenrows;
	int screencols;
	struct termios og_termios;
};

struct editorConfig E;

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

void editorDrawRows(struct abuf *ab) {
	int y;
	for (y = 0; y < E.screenrows; y++) {
		if (y == E.screenrows / 3) {
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

		// The K command [Erase In Line](https://vt100.net/docs/vt100-ug/chapter3.html#EL)
		// erases part of the current line. Its argument is analogous to the J command's argument: 2 erases the whole line,
		// 1 erases the part of the line to the left of the cursor, and 0 erases the part of the line to the right of the
		// cursor. 0 is the default argument, and that's what we want
		abAppend(ab, "\x1b[K", 3);
		if (y < E.screenrows - 1) {
			abAppend(ab, "\r\n", 2);
		}
	}
}

void editorRefreshScreen() {
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

	char buf[32];
	snprintf(buf, sizeof(buf), "\x1b[%d;%dH", E.cy + 1, E.cx + 1);
	abAppend(&ab, buf, strlen(buf));

	// This escape sequence is 6 bytes long, and uses the h command [Set Mode](https://vt100.net/docs/vt100-ug/chapter3.html#SM)
	// and causes one or more modes to be set within the terminal as specified by each selective parameter in the parameter string
	abAppend(&ab, "\x1b[?25h", 6);

	write(STDOUT_FILENO, ab.b, ab.len);
	abFree(&ab);
}

/*** input ***/

void editorMoveCursor(int key) {
	switch (key) {
		case ARROW_LEFT:
			if (E.cx != 0) {
				E.cx--;
			}
			break;
		case ARROW_RIGHT:
			if (E.cx != E.screencols - 1) {
				E.cx++;
			}
			break;
		case ARROW_UP:
			if (E.cy != 0) {
				E.cy--;
			}
			break;
		case ARROW_DOWN:
			if (E.cy != E.screenrows - 1) {
				E.cy++;
			}
			break;
	}
}

void editorProcessKeypress() {
	int c = editorReadKey();

	switch (c) {
		case CTRL_KEY('q'):
			write(STDOUT_FILENO, "\x1b[2J", 4);
			write(STDOUT_FILENO, "\x1b[H", 3);
			exit(0);
			break;

		case HOME_KEY:
			E.cx = 0;
			break;

		case END_KEY:
			E.cx = E.screencols - 1;
			break;
		
		case PAGE_UP:
		case PAGE_DOWN:
			{
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
	}
}

/*** init ***/

void initEditor() {
	E.cx = 0;
	E.cy = 0;

	if (getWindowSize(&E.screenrows, &E.screencols) == -1) die("getWindowSize");
}

int main() {
	enableRawMode();
	initEditor();

	while (1) {
		editorRefreshScreen();
		editorProcessKeypress();
	}

	return 0;
}
