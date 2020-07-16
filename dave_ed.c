#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <termios.h>

// Defines
#define DAVE_ED_VERSION "0.0.1"
#define CTRL_KEY(k) ((k) & 0x1f)

enum EditorKey {
	ARROW_LEFT = 1000,
	ARROW_RIGHT = 1001,
	ARROW_UP = 1002,
	ARROW_DOWN = 1003,
	PAGE_UP,
	PAGE_DOWN
};

// Data
struct EditorConfig {
	int cx, cy;
	int screen_rows;
	int screen_cols;
	struct termios orig_termios;
};

struct EditorConfig Ed;

// Terminal
void die(const char *s) {
	write(STDOUT_FILENO, "\x1b[2J", 4);
  	write(STDOUT_FILENO, "\x1b[H", 3);

	perror(s);
	exit(1);
}

void disable_raw_mode() {
	if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &Ed.orig_termios) == -1)
		die("tcsetattr");
}

void enable_raw_mode() {
	if (tcgetattr(STDIN_FILENO, &Ed.orig_termios) == -1) die("tcgetattr");
	atexit(disable_raw_mode);

	struct termios raw = Ed.orig_termios;
	raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
	raw.c_oflag &= ~(OPOST);
	raw.c_cflag &= ~(CS8);
	raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
	raw.c_cc[VMIN] = 0;
	raw.c_cc[VTIME] = 1;

	if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) die("tcsetattr");
}

int editor_read_key() {
	int nread;
	char c;
	while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
		if (nread == -1 && errno != EAGAIN) die("read");
	}

	if (c == '\x1b') {
		char sequence[3];

		if (read(STDIN_FILENO, &sequence[0], 1) != 1) return '\x1b';
		if (read(STDIN_FILENO, &sequence[1], 1) != 1) return '\x1b';

		if (sequence[0] == '[') {
			if (sequence[1] >= '0' && sequence[1] <= '9') {
				if (read(STDIN_FILENO, &sequence[2], 1) != 1) return '\x1b';
				if (sequence[2] == '*') {
					switch (sequence[1]) {
						case '5': return PAGE_UP;
						case '6': return PAGE_DOWN;
					}
				}
			} else {
				switch (sequence[1]) {
					case 'A': return ARROW_UP;
					case 'B': return ARROW_DOWN;
					case 'C': return ARROW_RIGHT;
					case 'D': return ARROW_LEFT;
				}
			}
		}

		return '\x1b';
	} else {
		return c;
	}
}

int get_cursor_position(int *rows, int *cols) {
	char buf[32];
	unsigned int i = 0;

	if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) return -1;

	while (i < sizeof(buf) - 1) {
		if (read(STDIN_FILENO, &buf[i], 1) != 1) break;
		if (buf[i] == 'R') break;
		i++;
	}

	buf[i] = '\0';

	if (buf[0] != '\x1b' || buf[1] != '[') return -1;

	return 0;
}

int get_window_size(int *rows, int *cols) {
	struct winsize ws;

	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
		if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) return -1;
		return get_cursor_position(rows, cols);
	} else {
		*cols = ws.ws_col;
		*rows = ws.ws_row;

		return 0;
	}
}

// Append Buffer
struct ABuf {
	char *buffer;
	int len;
};

#define ABUF_INIT {NULL, 0}

void abuf_append(struct ABuf *ab, const char *s, int len) {
	char *new = realloc(ab -> buffer, ab -> len + len);

	if (new == NULL) return;
	memcpy(&new[ab -> len], s, len);
	ab -> buffer = new;
	ab -> len += len;
}

void abuf_free(struct ABuf *ab) {
	free(ab -> buffer);
}

// Output
void editor_draw_rows(struct ABuf *ab) {
	int y = 0;

	for (y = 0; y < Ed.screen_rows; y++) {
		if (y == Ed.screen_rows / 3) {
			char welcome[80];
			int welcome_len = snprintf(welcome, sizeof(welcome), 
				"DaveEd -- Version %s", DAVE_ED_VERSION);

			if (welcome_len > Ed.screen_cols) welcome_len = Ed.screen_cols;

			int padding = (Ed.screen_cols - welcome_len) / 2;

			if (padding) {
				abuf_append(ab, "*", 1);
				padding--;
			}

			while (padding--) abuf_append(ab, " ", 1);

			abuf_append(ab, welcome, welcome_len);
		} else {
			abuf_append(ab, "*", 1);
		}

		abuf_append(ab, "\x1b[K", 3);
		if (y < Ed.screen_rows - 1) {
			abuf_append(ab, "\r\n", 2);
		}
	}
}

void editor_refresh_screen() {
	struct ABuf ab = ABUF_INIT;

	abuf_append(&ab, "\x1b[?25l", 6);
	abuf_append(&ab, "\x1b[H", 3);

	editor_draw_rows(&ab);

	char buf[32];
	snprintf(buf, sizeof(buf), "\x1b[%d;%dH", Ed.cy + 1, Ed.cx + 1);
	abuf_append(&ab, buf, strlen(buf));

	abuf_append(&ab, "\x1b[H", 3);
	abuf_append(&ab, "\x1b[?25l", 6);

	write(STDOUT_FILENO, ab.buffer, ab.len);
	abuf_free(&ab);
}

// Input
void editor_move_cursor(int key) {
	switch (key) {
	case ARROW_LEFT:
		if (Ed.cx != 0) {
			Ed.cx--;
		}
		
		break;

	case ARROW_RIGHT:
		if (Ed.cx != Ed.screen_cols - 1) {
			Ed.cx++;
		}
		
		break;

	case ARROW_UP:
		if (Ed.cy != 0) {
			Ed.cy--;
		}
		
		break;

	case ARROW_DOWN:
		if (Ed.cy != Ed.screen_rows - 1) {
			Ed.cy++;
		}
		
		break;
	}
}

void editor_process_keypress() {
	int c = editor_read_key();

	switch(c) {
		case CTRL_KEY('q'):
			write(STDOUT_FILENO, "\x1b[2J", 4);
	      	write(STDOUT_FILENO, "\x1b[H", 3);

			exit(0);
			break;

		case PAGE_UP:
		case PAGE_DOWN:
			{
				int times = Ed.screen_rows;
				while (times--)
					editor_move_cursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
			}
			break;

		case ARROW_UP:
		case ARROW_DOWN:
		case ARROW_LEFT:
		case ARROW_RIGHT:
			editor_move_cursor(c);
			break;
	}
}

// Init
void init_editor() {
	Ed.cx = 0;
	Ed.cy = 0;

	if (get_window_size(&Ed.screen_rows, &Ed.screen_cols) == -1) die("get_window_size");
}
int main() {
	enable_raw_mode();
	init_editor();

	while (1) {
		editor_refresh_screen();
		editor_process_keypress();
	}

	return 0;
}
