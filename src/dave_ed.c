#define _GNU_SOURCE
#define _DEFAULT_SOURCE
#define _BSD_SOURCE

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <unistd.h>
#include <termios.h>
#include <time.h>

// Defines
#define DAVE_ED_VERSION "0.0.1"
#define DAVE_ED_TAB_STOP 8
#define CTRL_KEY(k) ((k) & 0x1f)

enum EditorKey {
	ARROW_LEFT = 1000,
	ARROW_RIGHT,
	ARROW_UP,
	ARROW_DOWN,
	DELETE_KEY,
	HOME_KEY,
	END_KEY,
	PAGE_UP,
	PAGE_DOWN
};

// Data
typedef struct RowStore {
	int size;
	int rsize;
	char *chars;
	char *render;
} rstore;

struct EditorConfig {
	int cx, cy;
	int rx;
	int row_offset;
	int column_offset;
	int screen_rows;
	int screen_cols;
	int num_rows;
	rstore *row;
	char *file_name;
	char status_message[80];
	time_t status_message_time;
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
	if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &Ed.orig_termios) == -1) die("tcsetattr");
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
						case '1': return HOME_KEY;
						case '3': return DELETE_KEY;
						case '4': return END_KEY;
						case '5': return PAGE_UP;
						case '6': return PAGE_DOWN;
						case '7': return HOME_KEY;
						case '8': return END_KEY;
					}
				}
			} else {
				switch (sequence[1]) {
					case 'A': return ARROW_UP;
					case 'B': return ARROW_DOWN;
					case 'C': return ARROW_RIGHT;
					case 'D': return ARROW_LEFT;
					case 'H': return HOME_KEY;
					case 'F': return END_KEY;
				}
			}
		} else if (sequence[0] == 'O') {
			switch (sequence[1]) {
				case 'H': return HOME_KEY;
				case 'F': return END_KEY;
			}
		}

		return '\x1b';
	} else {
		return c;
	}
}

int get_cursor_position(int *rows, int *cols) {
	char buffer[32] = {0};
	unsigned int i = 0;
	rows = 0;
	cols = 0;

	if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) return -1;

	while (i < sizeof(buffer) - 1) {
		if (read(STDIN_FILENO, &buffer[i], 1) != 1) break;
		if (buffer[i] == 'R') break;
		i++;
	}

	buffer[i] = '\0';

	if (buffer[0] != '\x1b' || buffer[1] != '[') return -1;

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

// Row Operations
int editor_row_cx_to_rx(rstore *row, int cx) {
	int rx = 0;
	int j = -1;
	for (j = 0; j < cx; j++) {
		if (row -> chars[j] == '\t')
			rx += (DAVE_ED_TAB_STOP - 1) - (rx % DAVE_ED_TAB_STOP);
		rx++;
	}

	return rx;
}

void editor_update_row(rstore *row) {
	int tabs = 0;
	int j = -1;
	for (j = 0; j < row -> size; j++)
		if (row -> chars[j] == '\t') tabs++;

	free(row -> render);
	row -> render = malloc(row -> size + tabs * (DAVE_ED_TAB_STOP - 1) + 1);

	int idx = 0;
	for (j = 0; j < row -> size; j++) {
		if (row -> chars[j] == '\t') {
			row -> render[idx++] = ' ';
			while (idx % DAVE_ED_TAB_STOP != 0) row -> render[idx++] = ' ';
		} else {
			row -> render[idx++] = row -> chars[j];
		}
	}

	row -> render[idx] = '\0';
	row -> rsize = idx;
}

void editor_append_row(char *s, size_t len) {
	Ed.row = realloc(Ed.row, sizeof(rstore) * (Ed.num_rows + 1));

	int at = Ed.num_rows;
	Ed.row[at].size = len;
	Ed.row[at].chars = malloc(len + 1);
	memcpy(Ed.row[at].chars, s, len);
	Ed.row[at].chars[len] = '\0';

	Ed.row[at].rsize = 0;
	Ed.row[at].render = NULL;
	editor_update_row(&Ed.row[at]);

	Ed.num_rows++;
}

// File I/O
void editor_open(char *file_name) {
	free(Ed.file_name);
	Ed.file_name = strdup(file_name);

	FILE *file_pointer = fopen(file_name, "r");
	if (!file_pointer) die("fopen");

	char *line = NULL;
	size_t line_cap = 0;
	ssize_t line_len = 0;
	while((line_len = getline(&line, &line_cap, file_pointer)) != -1) {
		while (line_len > 0 && (line[line_len - 1] == '\n' ||
								line[line_len - 1] == '\r' ))
			line_len--;

		editor_append_row(line, line_len);
	}

	free(line);
	fclose(file_pointer);
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
void editor_scroll() {
	Ed.rx = 0;
	if (Ed.cy < Ed.num_rows) {
		Ed.rx = editor_row_cx_to_rx(&Ed.row[Ed.cy], Ed.cx);

	}

	if (Ed.cy < Ed.row_offset) {
		Ed.row_offset = Ed.cy;
	}

	if (Ed.cy >= Ed.row_offset + Ed.screen_rows) {
		Ed.row_offset = Ed.cy - Ed.screen_rows + 1;
	}

	if (Ed.rx < Ed.column_offset) {
		Ed.column_offset = Ed.rx;
	}

	if (Ed.rx >= Ed.column_offset + Ed.screen_cols) {
		Ed.column_offset = Ed.rx - Ed.screen_cols + 1;
	}
}

void editor_draw_rows(struct ABuf *ab) {
	int y = 0;

	for (y = 0; y < Ed.screen_rows; y++) {
		int file_row = y + Ed.row_offset;

		if (file_row >= Ed.num_rows){
			if (Ed.num_rows == 0 && y == Ed.screen_rows  / 3) {
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
		} else {
			int length = Ed.row[file_row].rsize - Ed.column_offset;

			if (length < 0) length = 0; 
			if (length > Ed.screen_cols) length = Ed.screen_cols;
			abuf_append(ab, &Ed.row[file_row].render[Ed.column_offset], length);
		}

		abuf_append(ab, "\x1b[K", 3);
		abuf_append(ab, "\r\n", 2);
	}
}

void editor_draw_status_bar(struct ABuf *ab) {
	abuf_append(ab, "\x1b[7m", 4);

	char status[80], rstatus[80];
	int len = snprintf(status, sizeof(status), "%.20s - %d lines",
		Ed.file_name ? Ed.file_name : "[No File Name]", Ed.num_rows);
	int rlen = snprintf(rstatus, sizeof(rstatus), "%d/%d", Ed.cy + 1, Ed.num_rows);

	if (len > Ed.screen_cols) len = Ed.screen_cols;
	abuf_append(ab, status, len);

	while (len < Ed.screen_cols) {
		if (Ed.screen_cols - len == rlen) {
			abuf_append(ab, rstatus, rlen);
			break;
		} else {
			abuf_append(ab, " ", 1);
			len++;
		}
	}

	abuf_append(ab, "\x1b[m", 3);
}

void editor_refresh_screen() {
	editor_scroll();

	struct ABuf ab = ABUF_INIT;

	abuf_append(&ab, "\x1b[?25l", 6);
	abuf_append(&ab, "\x1b[H", 3);

	editor_draw_rows(&ab);
	editor_draw_status_bar(&ab);

	char buffer[32];
	snprintf(buffer, sizeof(buffer), "\x1b[%d;%dH", (Ed.cy - Ed.row_offset) + 1, 
													(Ed.rx - Ed.column_offset) + 1);
	abuf_append(&ab, buffer, strlen(buffer));
	abuf_append(&ab, "\x1b[?25h", 6);

	write(STDOUT_FILENO, ab.buffer, ab.len);
	abuf_free(&ab);
}

void editor_set_status_message(const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(Ed.status_message, sizeof(Ed.status_message), fmt, ap);
	va_end(ap);
	Ed.status_message_time = time(NULL);
}

// Input
void editor_move_cursor(int key) {
	rstore *row = (Ed.cy >= Ed.num_rows) ? NULL : &Ed.row[Ed.cy];

	switch (key) {
	case ARROW_LEFT:
		if (Ed.cx != 0) {
			Ed.cx--;
		} else if (Ed.cy > 0) {
			Ed.cy--;
			Ed.cx = Ed.row[Ed.cy].size;
		}
		
		break;

	case ARROW_RIGHT:
		if (row && Ed.cx < row -> size) {
			Ed.cx++;
		} else if (row && Ed.cx == row -> size) {
			Ed.cy++;
			Ed.cx = 0;
		}
		
		break;

	case ARROW_UP:
		if (Ed.cy != 0) {
			Ed.cy--;
		}
		
		break;

	case ARROW_DOWN:
		if (Ed.cy < Ed.num_rows) {
			Ed.cy++;
		}
		
		break;
	}

	row = (Ed.cy >= Ed.num_rows) ? NULL : &Ed.row[Ed.cy];
	int row_length = row ? row -> size : 0;
	if (Ed.cx > row_length) {
		Ed.cx = row_length;
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

		case HOME_KEY:
			Ed.cx = 0;
			break;

		case END_KEY:
			if (Ed.cy < Ed.num_rows)
				Ed.cx = Ed.row[Ed.cy].size;
			break;

		case PAGE_UP:
		case PAGE_DOWN:
			{
				if (c == PAGE_UP) {
					Ed.cy = Ed.row_offset;
				} else if (c == PAGE_DOWN) {
					Ed.cy = Ed.row_offset + Ed.screen_rows - 1;
					if (Ed.cy > Ed.num_rows) Ed.cy = Ed.num_rows;
				}


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
	Ed.rx = 0;
	Ed.row_offset = 0;
	Ed.column_offset = 0;
	Ed.num_rows = 0;
	Ed.row = NULL;
	Ed.file_name = NULL;
	Ed.status_message[0] = '\0';
	Ed.status_message_time = 0;

	if (get_window_size(&Ed.screen_rows, &Ed.screen_cols) == -1) die("get_window_size");
	Ed.screen_rows -= 1;
}

int main(int argc, char *argv[]) {
	enable_raw_mode();
	init_editor();
	if (argc >= 2) {
		editor_open(argv[1]);
	}

	editor_set_status_message("HELP: Ctrl-Q to Quit");

	while (1) {
		editor_refresh_screen();
		editor_process_keypress();
	}

	return 0;
}
