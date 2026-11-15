CC = gcc
CFLAGS = -std=c11 -O2 -g -Wall -Wextra -Wpedantic
THREAD_FLAGS = -pthread

.PHONY: all clean test sanitize tutorial
all: proxy_server proxy_client

proxy_server: proxy_server.c net.c net.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(THREAD_FLAGS) proxy_server.c net.c $(LDFLAGS) -o $@

proxy_client: proxy_client.c net.c net.h
	$(CC) $(CPPFLAGS) $(CFLAGS) proxy_client.c net.c $(LDFLAGS) -o $@

# Original tutorial implementation, preserved for comparison only.
tutorial: proxy_server_with_cache.c proxy_parse.c proxy_parse.h
	g++ -g -Wall -pthread proxy_server_with_cache.c proxy_parse.c -o tutorial_proxy

test: all
	python3 tests/integration.py

sanitize:
	$(MAKE) clean
	$(MAKE) CFLAGS='-std=c11 -O1 -g -Wall -Wextra -Wpedantic -fsanitize=address,undefined -fno-omit-frame-pointer' LDFLAGS='-fsanitize=address,undefined' all

clean:
	rm -f proxy_server proxy_client tutorial_proxy *.o
