CC=gcc
CFLAGS=-Wall -g -c
SOURCES=dave_ed.c
OBJECTS=$(SOURCES:.c=.o)
TARGET=DaveEd

all: $(SOURCES) $(TARGET)

clean:
	rm -f $(TARGET) $(OBJECTS)

$(TARGET): $(OBJECTS)
	$(CC) $(OBJECTS) -o $@

DaveEd.o: dave_ed.c
	$(CC) $(CFLAGS) dave_ed.c -o $@