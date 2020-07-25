#define _GNU_SOURCE
#define _DEFAULT_SOURCE
#define _BSD_SOURCE

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
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
#define DAVE_ED_QUIT_WARNINGS 2

#define CTRL_KEY(k) ((k) & 0x1f)

enum EditorKey {
	BACKSPACE = 127,
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

enum EditorHighlight {
	HL_NORMAL = 0,
	HL_COMMENT,
	HL_KEYWORD1,
	HL_KEYWORD2,
	HL_STRING,
	HL_NUMBER,
	HL_MATCH
};

#define HL_HIGHLIGHT_NUMBERS (1<<0)
#define HL_HIGHLIGHT_STRINGS (1<<1)

// Data
struct EditorSyntax {
	char *file_type;
	char **file_match;
	char **keywords;
	char *single_line_comment_start;
	int flags;
};

typedef struct RowStore {
	int size;
	int rsize;
	char *chars;
	char *render;
	unsigned char *highlight;
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
	int unsaved_changes_flag;
	char *file_name;
	char status_message[80];
	time_t status_message_time;
	struct EditorSyntax *syntax;
	struct termios orig_termios;
};

struct EditorConfig Ed;

// FileTypes
char *C_HL_extensions[] = { ".c", ".h", ".cpp", NULL };
char *C_HL_keywords[] = {
	"switch", "if", "while", "for", "break", "continue", "return", "else", "struct", "union", "typedef", "static", "enum", "class", "case", "int|", "long|", "double|", "float|", "char|", "unsigned|", "signed|", "void|", NULL
};

// Highlight Database
struct EditorSyntax HLDB[] = {
	{
		"c",
		C_HL_extensions,
		C_HL_keywords,
		"//",
		HL_HIGHLIGHT_NUMBERS | HL_HIGHLIGHT_STRINGS
	},
};

#define HLDB_ENTRIES  (sizeof(HLDB) / sizeof(HLDB[0]))

// Prototypes
void editor_set_status_message(const char *fmt, ...);
void editor_refresh_screen();
char *editor_prompt(char *prompt, void (*callback)(char *, int));

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

// Syntax Highlighting
int is_seperator(int c) {
	return isspace(c) || c == '\0' || strchr(",.()+-/*=~%<>[];", c) != NULL;
}

void editor_update_syntax(rstore *row) {
	row -> highlight = realloc(row -> highlight, row -> rsize);
	memset(row -> highlight, HL_NORMAL, row -> rsize);

	if (Ed.syntax == NULL) return;

	char **keywords = Ed.syntax -> keywords;
	char *scs = Ed.syntax -> single_line_comment_start;
	int scs_len = scs ? strlen(scs) : 0;
	int previous_seperator = 1;
	int in_string = 0;
	int i = 0;
	while (i < row -> rsize) {
		char c = row -> render[i];
		unsigned char previous_highlight = (i > 0) ? row -> highlight[i - 1] : HL_NORMAL;

		if (scs_len && !in_string) {
			if (!strncmp(&row -> render[i], scs, scs_len)) {
				memset(&row -> highlight[i], HL_COMMENT, row -> rsize - i);

				break;
			}
		}

		if (Ed.syntax -> flags & HL_HIGHLIGHT_STRINGS) {
			if (in_string) {
				row -> highlight[i] = HL_STRING;
				if (c == '\\' && i + 1 < row -> rsize) {
					row -> highlight[i + 1] = HL_STRING;
					i += 2;

					continue;
				}

				if (c == in_string) in_string = 0;
				i++;
				previous_seperator = 1;

				continue;
			} else {
				if (c == '"' || c == '\'') {
					in_string = c;
					row -> highlight[i] = HL_STRING;
					i++;

					continue;
				}
			}
		}

		if (Ed.syntax -> flags & HL_HIGHLIGHT_NUMBERS) {
			if ((isdigit(c) && (previous_seperator || previous_highlight == HL_NUMBER)) || (c == '.' && previous_highlight == HL_NUMBER)) {
				row -> highlight[i] = HL_NUMBER;
				i++;
				previous_seperator = 0;
				continue;
			}
		}

		if (previous_seperator) {
			int j = 0;
			for (j = 0; keywords[j]; j++) {
				int klen = strlen(keywords[j]);
				int keyword_two = keywords[j][klen - 1] == '|';
				if (keyword_two) klen--;

				if (!strncmp(&row -> render[i], keywords[j], klen) && is_seperator(row -> render[i + klen])) {
					memset(&row -> highlight[i], keyword_two ? HL_KEYWORD2 : HL_KEYWORD1, klen);
					i += klen;

					break;
				}
			}

			if (keywords[j] != NULL) {
				previous_seperator = 0;

				continue;
			}
		}

		previous_seperator = is_seperator(c);
		i++;
	}
}

int editor_syntax_to_color(int highlight) {
	switch (highlight) {
		case HL_COMMENT: return 36;
		case HL_KEYWORD1: return 34;
		case HL_KEYWORD2: return 32;
		case HL_STRING: return 35;
		case HL_NUMBER: return 31;
		case HL_MATCH: return 34;
		default: return 37;
	}
}

void editor_select_syntax_highlight() {
	Ed.syntax = NULL;
	if (Ed.file_name == NULL) return;

	char *ext = strrchr(Ed.file_name, '.');

	for (unsigned int j = 0; j < HLDB_ENTRIES; j++) {
		struct EditorSyntax *s = &HLDB[j];
		unsigned int i = 0;

		while (s -> file_match[i]) {
			int is_ext = (s -> file_match[i][0] == '.');
			if ((is_ext && ext && !strcmp(ext, s -> file_match[i])) || (!is_ext && strstr(Ed.file_name, s -> file_match[i]))) {
				Ed.syntax = s;

				int file_row;
				for (file_row = 0; file_row < Ed.num_rows; file_row++) {
					editor_update_syntax(&Ed.row[file_row]);
				}

				return;
			}

			i++;
		}
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

int editor_row_rx_to_cx(rstore *row, int rx) {
	int cur_rx = 0;
	int cx = 0;
	for (cx = 0; cx < row -> size; cx++) {
		if (row -> chars[cx] == '\t')
			cur_rx += (DAVE_ED_TAB_STOP - 1) - (cur_rx % DAVE_ED_TAB_STOP);
		cur_rx++;

		if (cur_rx > rx) return cx;
	}

	return cx;
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

	editor_update_syntax(row);
}

void editor_insert_row(int at, char *s, size_t len) {
	if (at < 0 || at > Ed.num_rows) return;

	Ed.row = realloc(Ed.row, sizeof(rstore) * (Ed.num_rows + 1));
	memmove(&Ed.row[at + 1], &Ed.row[at], sizeof(rstore) * (Ed.num_rows - at));

	Ed.row[at].size = len;
	Ed.row[at].chars = malloc(len + 1);
	memcpy(Ed.row[at].chars, s, len);
	Ed.row[at].chars[len] = '\0';

	Ed.row[at].rsize = 0;
	Ed.row[at].render = NULL;
	Ed.row[at].highlight = NULL;
	editor_update_row(&Ed.row[at]);

	Ed.num_rows++;
	Ed.unsaved_changes_flag++;
}

void editor_free_row(rstore *row) {
	free(row -> render);
	free(row -> chars);
	free(row -> highlight);
}

void editor_delete_row(int at) {
	if (at < 0 || at >= Ed.num_rows) return;
	editor_free_row(&Ed.row[at]);
	memmove(&Ed.row[at], &Ed.row[at + 1], sizeof(rstore) * (Ed.num_rows - at - 1));
	Ed.num_rows--;
	Ed.unsaved_changes_flag++;
}

void editor_row_insert_character(rstore *row, int at, int c) {
	if (at < 0 || at > row -> size) at = row -> size;
	row -> chars = realloc(row -> chars, row -> size + 2);
	memmove(&row -> chars[at + 1], &row -> chars[at], row -> size - at + 1);
	row -> size++;
	row -> chars[at] = c;
	editor_update_row(row);
	Ed.unsaved_changes_flag++;
}

void editor_row_append_string(rstore *row, char *s, size_t len) {
	row -> chars = realloc(row -> chars, row -> size + len + 1);
	memcpy(&row -> chars[row -> size], s, len);
	row -> size += len;
	row -> chars[row -> size] = '\0';
	editor_update_row(row);
	Ed.unsaved_changes_flag++;
}

void editor_row_delete_character(rstore *row, int at) {
	if (at < 0 || at >= row -> size) return;
	memmove(&row -> chars[at], &row -> chars[at + 1], row -> size - at);
	row -> size--;
	editor_update_row(row);
	Ed.unsaved_changes_flag++;
}

// Editor Operations
void editor_insert_character(int c) {
	if (Ed.cy == Ed.num_rows) {
		editor_insert_row(Ed.num_rows, "", 0);
	}

	editor_row_insert_character(&Ed.row[Ed.cy], Ed.cx, c);
	Ed.cx++;
}

void editor_insert_new_line() {
	if (Ed.cx == 0) {
		editor_insert_row(Ed.cy, "", 0);
	} else {
		rstore *row = &Ed.row[Ed.cy];
		editor_insert_row(Ed.cy + 1, &row -> chars[Ed.cx], row -> size - Ed.cx);
		row = &Ed.row[Ed.cy];
		row -> size = Ed.cx;
		row -> chars[row -> size] = '\0';
		editor_update_row(row);
	}

	Ed.cy++;
	Ed.cx = 0;
}

void editor_delete_character() {
	if (Ed.cy == Ed.num_rows) return;
	if (Ed.cx == 0 && Ed.cy == 0) return;

	rstore *row = &Ed.row[Ed.cy];
	if (Ed.cx > 0) {
		editor_row_delete_character(row, Ed.cx - 1);
		Ed.cx--;
	} else {
		Ed.cx = Ed.row[Ed.cy - 1].size;
		editor_row_append_string(&Ed.row[Ed.cy - 1], row -> chars, row -> size);
		editor_delete_row(Ed.cy);
		Ed.cy--;
	}
}

// File I/O
char *editor_rows_to_string(int *bufferlen) {
	int totlen = 0;
	int j = -1;

	for (j = 0; j < Ed.num_rows; j++)
		totlen += Ed.row[j].size + 1;
	*bufferlen = totlen;

	char *buf = malloc(totlen);
	char *p = buf;

	for (j = 0; j < Ed.num_rows; j++) {
		memcpy(p, Ed.row[j].chars, Ed.row[j].size);
		p += Ed.row[j].size;
		*p = '\n';
		p++;
	}

	return buf;
}

void editor_open(char *file_name) {
	free(Ed.file_name);
	Ed.file_name = strdup(file_name);

	editor_select_syntax_highlight();

	FILE *file_pointer = fopen(file_name, "r");
	if (!file_pointer) die("fopen");

	char *line = NULL;
	size_t line_cap = 0;
	ssize_t line_len = 0;
	while((line_len = getline(&line, &line_cap, file_pointer)) != -1) {
		while (line_len > 0 && (line[line_len - 1] == '\n' ||
								line[line_len - 1] == '\r' ))
			line_len--;

		editor_insert_row(Ed.num_rows, line, line_len);
	}

	free(line);
	fclose(file_pointer);
	Ed.unsaved_changes_flag = 0;
}

void editor_save() {
	if (Ed.file_name == NULL) {
		Ed.file_name = editor_prompt("Save as: %s (ESC to Cancel)", NULL);
		if (Ed.file_name == NULL) {
			editor_set_status_message("Save Aborted");

			return;
		}

		editor_select_syntax_highlight();
	}

	int length = 0;
	char *buffer = editor_rows_to_string(&length);
	int fd = open(Ed.file_name, O_RDWR | O_CREAT, 0644);

	if (fd != -1) {
		if (ftruncate(fd, length)  != -1) {
			if (write(fd, buffer, length) == length) {
				close(fd);
				free(buffer);
				Ed.unsaved_changes_flag = 0;
				editor_set_status_message("%d Bytes Written to Disk", length);

				return;
			}
		}

		close(fd);
	}

	free(buffer);
	editor_set_status_message("Unable to Save! I/O Error: %s", strerror(errno));
}

// Find
void editor_find_callback(char *query, int key) {
	static int last_match = -1;
	static int direction = 1;
	static int saved_highlighted_line = 0;
	static char *saved_highlight = NULL;

	if (saved_highlight) {
		memcpy(Ed.row[saved_highlighted_line].highlight, saved_highlight, Ed.row[saved_highlighted_line].rsize);
		free(saved_highlight);
		saved_highlight = NULL;
	}

	if (key == '\r' || key == '\x1b') {
		last_match = -1;
		direction = 1;
		return;
	} else if (key == ARROW_RIGHT || key == ARROW_DOWN) {
		direction = 1;
	} else if (key == ARROW_LEFT || key == ARROW_UP) {
		direction = -1;
	} else {
		last_match = -1;
		direction = 1;
	}

	if (last_match == -1) direction = 1;
	int current = last_match;
	int i;
	for (i = 0; i < Ed.num_rows; i++) {
		current += direction;
		if (current == -1) current = Ed.num_rows - 1;
		else if (current == Ed.num_rows) current = 0;

		rstore *row = &Ed.row[current];
		char *match = strstr(row -> render, query);

		if (match) {
			last_match = current;
			Ed.cy = current;
			Ed.cx = editor_row_rx_to_cx(row, match - row -> render);
			Ed.row_offset = Ed.num_rows;

			saved_highlighted_line = current;
			saved_highlight = malloc(row -> rsize);
			memcpy(saved_highlight, row -> highlight, row -> rsize);
			memset(&row -> highlight[match - row -> render], HL_MATCH, strlen(query));
			break;
		}
	}
}

void editor_find() {
	int saved_cx = Ed.cx;
	int saved_cy = Ed.cy;
	int saved_column_offset = Ed.column_offset;
	int saved_row_offset = Ed.row_offset;

	char *query = editor_prompt("Search: %s (ESC Cancel/Arrows/Enter Confirm)", editor_find_callback);
	if (query) {
		free(query);
	} else {
		Ed.cx = saved_cx;
		Ed.cy = saved_cy;
		Ed.column_offset = saved_column_offset;
		Ed.row_offset = saved_row_offset;
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

			char *c = &Ed.row[file_row].render[Ed.column_offset];
			unsigned char *highlight = &Ed.row[file_row].highlight[Ed.column_offset];
			int current_color = -1;
			int j = 0;
			for (j = 0; j < length; j++) {
				if (highlight[j] == HL_NORMAL) {
					if (current_color != -1) {
						abuf_append(ab, "\x1b[39m", 5);
						current_color = -1;
					}

					abuf_append(ab, &c[j], 1);
				} else {
					int color = editor_syntax_to_color(highlight[j]);
					if (color != current_color) {
						current_color = color;
						char buf[16];
						int clen = snprintf(buf, sizeof(buf), "\x1b[%dm", color);
						abuf_append(ab, buf, clen);
					}
					
					abuf_append(ab, &c[j], 1);
				}
			}

			abuf_append(ab, "\x1b[39m", 5);
		}

		abuf_append(ab, "\x1b[K", 3);
		abuf_append(ab, "\r\n", 2);
	}
}

void editor_draw_status_bar(struct ABuf *ab) {
	abuf_append(ab, "\x1b[7m", 4);

	char status[80], rstatus[80];
	int len = snprintf(status, sizeof(status), "%.20s - %d lines %s", Ed.file_name ? Ed.file_name : "[No File Name]", Ed.num_rows, Ed.unsaved_changes_flag ? "(modified)" : "");
	int rlen = snprintf(rstatus, sizeof(rstatus), ".%s File Type | %d/%d", Ed.syntax ? Ed.syntax -> file_type : "File Type Empty", Ed.cy + 1, Ed.num_rows);

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
	abuf_append(ab, "\r\n", 2);
}

void editor_draw_message_bar(struct ABuf *ab) {
	abuf_append(ab, "\x1b[K", 3);

	int message_length = strlen(Ed.status_message);

	if (message_length > Ed.screen_cols) message_length = Ed. screen_cols;
	if (message_length && time(NULL) - Ed.status_message_time < 5)
		abuf_append(ab, Ed.status_message, message_length);
}

void editor_refresh_screen() {
	editor_scroll();

	struct ABuf ab = ABUF_INIT;

	abuf_append(&ab, "\x1b[?25l", 6);
	abuf_append(&ab, "\x1b[H", 3);

	editor_draw_rows(&ab);
	editor_draw_status_bar(&ab);
	editor_draw_message_bar(&ab);

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
char *editor_prompt(char *prompt, void (*callback)(char *, int)) {
	size_t bufsize = 128;
	char *buf = malloc(bufsize);

	size_t buflen = 0;
	buf[0] = '\0';

	while(1) {
		editor_set_status_message(prompt, buf);
		editor_refresh_screen();

		int c = editor_read_key();
		if (c == DELETE_KEY || c == CTRL_KEY('h') || c == BACKSPACE) {
			if (buflen != 0) buf[--buflen] = '\0';
		} else if (c == '\x1b') {
			editor_set_status_message("");
			if (callback) callback(buf, c);
			free(buf);

			return NULL;
		} else if (c == '\r') {
			if (buflen != 0) {
				editor_set_status_message("");
				if (callback) callback(buf, c);
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

		if (callback) callback(buf, c);
	}
}

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
	static int quit_times = DAVE_ED_QUIT_WARNINGS;
	int c = editor_read_key();

	switch(c) {
		case '\r':
			editor_insert_new_line();
			break;

		case CTRL_KEY('q'):
			if (Ed.unsaved_changes_flag && quit_times > 0) {
				editor_set_status_message("UNSAVED CHANGES!"
										" Really Quit? Press Ctrl-Q %d more times.", quit_times);
				quit_times--;

				return;
			}
			write(STDOUT_FILENO, "\x1b[2J", 4);
	      	write(STDOUT_FILENO, "\x1b[H", 3);
			exit(0);
			break;

		case CTRL_KEY('s'):
			editor_save();
			break;

		case HOME_KEY:
			Ed.cx = 0;
			break;

		case END_KEY:
			if (Ed.cy < Ed.num_rows)
				Ed.cx = Ed.row[Ed.cy].size;
			break;

		case CTRL_KEY('f'):
			editor_find();
			break;

		case BACKSPACE:
		case CTRL_KEY('h'):
		case DELETE_KEY:
			if (c == DELETE_KEY) editor_move_cursor(ARROW_RIGHT);
			editor_delete_character();
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

		case CTRL_KEY('l'):
		case '\x1b':
			break;

		default:
			editor_insert_character(c);
			break;
	}

	quit_times = DAVE_ED_QUIT_WARNINGS;
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
	Ed.unsaved_changes_flag = 0;
	Ed.file_name = NULL;
	Ed.status_message[0] = '\0';
	Ed.status_message_time = 0;
	Ed.syntax = NULL;

	if (get_window_size(&Ed.screen_rows, &Ed.screen_cols) == -1) die("get_window_size");
	Ed.screen_rows -= 2;
}

int main(int argc, char *argv[]) {
	enable_raw_mode();
	init_editor();
	if (argc >= 2) {
		editor_open(argv[1]);
	}

	editor_set_status_message("HELP: Ctrl-S to Save | Ctrl-Q to Quit | Ctrl-F = Find");

	while (1) {
		editor_refresh_screen();
		editor_process_keypress();
	}

	return 0;
}
