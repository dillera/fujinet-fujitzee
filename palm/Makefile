# FN Fujitzee for Palm OS. Builds in the palmdev Docker image (docker/Dockerfile).
IMAGE  = palmdev
DOCKER = docker run --rm --platform linux/amd64 -v $(CURDIR):/src -w /src $(IMAGE)

all: build/icon.bmp
	$(DOCKER) make -f app.mk

# The icon is drawn on the host; the toolchain image has no Python.
build/icon.bmp: tools/make_icon.py
	mkdir -p build
	python3 tools/make_icon.py $@

image:
	docker build --platform linux/amd64 -t $(IMAGE) docker

test:
	$(MAKE) -C tests

clean:
	rm -rf build
	$(MAKE) -C tests clean

.PHONY: all image test clean
