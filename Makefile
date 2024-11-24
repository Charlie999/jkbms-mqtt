jk: jk.c
	$(CC) jk.c -lmodbus -lmosquitto -o jk -DGIT_BRANCH=$(shell git rev-parse --abbrev-ref HEAD) -DGIT_VERSION=$(shell git rev-parse --short HEAD)

.PHONY: clean
clean:
	rm -f jk
