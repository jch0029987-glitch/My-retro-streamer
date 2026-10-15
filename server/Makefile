CC ?= clang
CFLAGS ?= -O3 -fPIC -Wall
LDFLAGS ?= -shared -llog

TARGET = libstream_server.so
SRCS = stream_server.c
OBJS = $(SRCS:.c=.o)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(OBJS) $(LDFLAGS) -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)
