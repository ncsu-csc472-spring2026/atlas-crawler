CFLAGS	= -ggdb -Wall -Wextra
BIN		=./bin/
CC		= gcc
LDLIBS	= -lcurl

$(BIN)atlas_crawler: atlas_crawler.c | $(BIN)
	$(CC) $(CFLAGS) atlas_crawler.c -o $(BIN)atlas_crawler $(LDLIBS)

$(BIN):
	mkdir -p $(BIN)

clean:
	rm -rf $(BIN)
