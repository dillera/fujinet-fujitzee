# Run inside the palmdev image (see Makefile).
APP     = Fujitzee
NAME    = "FN Fujitzee"
CREATOR = ADFZ
CC      = m68k-palmos-gcc
CFLAGS  = -O2 -Wall -palmos3.5 -Icommon -I. '-DAPP_CREATOR_STR="$(CREATOR)"'
SOURCES = $(APP).c fzwire.c common/fujibus.c common/fnlink.c common/fnnet.c

build/$(APP).prc: build/$(APP) build/resources.stamp
	build-prc -n $(NAME) -c $(CREATOR) -o $@ build/$(APP) build/*.bin

build/$(APP): $(SOURCES) *.h common/*.h
	$(CC) $(CFLAGS) -o $@ $(SOURCES)

build/resources.stamp: $(APP).rcp $(APP)Rsc.h build/icon.bmp
	pilrc -q -I build $(APP).rcp build
	touch $@
