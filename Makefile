audio-info: main.c	
	$(CC) -O3 -o $@ main.c -Wall -Wextra -Wconversion -Wshadow -Wimplicit-fallthrough -std=gnu99 `pkg-config --libs --cflags libavformat libavutil` -pthread
install: audio-info
	install audio-info /usr/bin/
clean:
	rm -f audio-info
