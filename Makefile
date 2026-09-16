TARGET = xl710_unlock

CC ?= gcc
CFLAGS ?= -Wall -Wextra -O2
INST_DIR ?= /usr/sbin

all: $(TARGET)

$(TARGET): $(TARGET).c
	$(CC) $(CFLAGS) $(LDFLAGS) $< -o $@

test: $(TARGET)
	./$(TARGET) -t

clean:
	rm -f $(TARGET)

install: $(TARGET)
	mkdir -p $(DESTDIR)$(INST_DIR)
	install -m 750 $(TARGET) $(DESTDIR)$(INST_DIR)

.PHONY: all test clean install
