/*** includes ***/

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>

/*** data ***/

struct termios og_termios;

/*** terminal ***/

void die(const char *s) {
	perror(s);
	exit(1);
}

void disableRawMode() {
	// Sets the terminal attributes for the file descriptor STDIN_FILENO, and the optional_actions
	// to TCSAFLUSH from the termios structure referenced by *termios_p
	if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &og_termios) == -1)
		die("tcsetattr");
}

void enableRawMode() {
	// Gets the terminal attributes for the file descriptor STDIN_FILENO from the termios structure
	// referenced by *termios_p
	if (tcgetattr(STDIN_FILENO, &og_termios) == -1) die("tcgetattr");
	atexit(disableRawMode);

	struct termios raw = og_termios;
	
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

/*** init ***/

int main() {
	enableRawMode();

	while (1) {
		char c = '\0';
		if (read(STDIN_FILENO, &c, 1) == -1 && errno != EAGAIN) die("read");
		if (iscntrl(c)) {
			printf("%d\r\n", c);
		} else {
			printf("%d ('%c')\r\n", c, c);
		}

		if (c == 'q') break;
	}

	return 0;
}
