# Host tests for the PicoGUS web server: real lwIP + web server code on a
# loopback interface. Needs a host C/C++ compiler, Python 3 and the Pico SDK
# (for its copy of lwIP).
#
#   make PICO_SDK_PATH=/path/to/pico-sdk test
#   make ui      the web page in a browser (needs Node.js with Playwright)
#   make card    a mock card to try the page on

PICO_SDK_PATH ?= $(error set PICO_SDK_PATH)
LWIP := $(PICO_SDK_PATH)/lib/lwip
SW := ../..
OUT := build

LWIP_SRC := $(addprefix $(LWIP)/src/core/, init.c def.c dns.c inet_chksum.c ip.c mem.c memp.c netif.c \
	pbuf.c raw.c stats.c sys.c tcp.c tcp_in.c tcp_out.c timeouts.c udp.c) \
	$(addprefix $(LWIP)/src/core/ipv4/, acd.c dhcp.c etharp.c icmp.c igmp.c ip4.c ip4_addr.c ip4_frag.c) \
	$(LWIP)/src/netif/ethernet.c
WEB_SRC := ../web_http.c ../web_upload.c ../web_assets.c
WWW := $(shell find ../www -type f)

DEFS := -DPICOGUS_WEB_MDNS=0 -DWEBSERVER=1 -DSOUND_MPU=1 -DSOUND_SB=1 -DSOUND_OPL=1 -DPICOGUS_MULTIFW=1
# test dir first: it overrides lwipopts.h and stubs the Pico SDK headers
INC := -I. -Istubs -I.. -I$(SW) -I$(LWIP)/src/include -I$(LWIP)/contrib/ports/unix/port/include -I$(OUT)
CFLAGS := -O1 -g -Wall -Wno-unused-function -Wno-unused-variable -fsanitize=address,undefined $(DEFS) $(INC)

$(OUT)/test_web: test_web.c defaults.c $(WEB_SRC) ../web_api.cpp $(OUT)/web_assets.h ../*.h ../lwipopts.h lwipopts.h
	gcc $(CFLAGS) -std=gnu11 -c test_web.c -o $(OUT)/test_web.o
	gcc $(CFLAGS) -std=gnu11 -c defaults.c -o $(OUT)/defaults.o
	gcc $(CFLAGS) -std=gnu11 -c ../web_http.c -o $(OUT)/web_http.o
	gcc $(CFLAGS) -std=gnu11 -c ../web_upload.c -o $(OUT)/web_upload.o
	gcc $(CFLAGS) -std=gnu11 -c ../web_assets.c -o $(OUT)/web_assets.o
	g++ $(CFLAGS) -std=gnu++17 -c ../web_api.cpp -o $(OUT)/web_api.o
	gcc $(CFLAGS) -std=gnu11 -w $(LWIP_SRC) $(OUT)/*.o -lstdc++ -o $@

$(OUT)/test_midi: test_midi.c $(SW)/mpu401/midi.c $(SW)/mpu401/export.h
	mkdir -p $(OUT)
	gcc $(CFLAGS) -std=gnu11 test_midi.c $(SW)/mpu401/midi.c -o $@

# the page as the firmware packs it, with two more files to test with
$(OUT)/web_assets.h: $(WWW) $(SW)/scripts/web_pack.py Makefile
	mkdir -p $(OUT)
	printf 'PicoGUS test file\n' > $(OUT)/hello.txt
	printf '\211PNG\r\n\032\n not really a picture' > $(OUT)/dot.png
	python3 $(SW)/scripts/web_pack.py ../www --header $@ --html $(OUT)/index.html \
		--extra /static/hello.txt=$(OUT)/hello.txt --extra /static/dot.png=$(OUT)/dot.png

test: $(OUT)/test_web $(OUT)/test_midi
	cd $(OUT) && ./test_midi
	cd $(OUT) && ./test_web
	python3 -c "import json; d = json.load(open('$(OUT)/status.json')); print('status JSON ok:', len(d['s']), 'settings')"

# The web page in a browser, against a mock card: once with the keyboard only
# and once with the mouse only. Needs Node.js with Playwright; not part of "test".
ui:
	node ui_test.js

# The mock card by itself, to work on the page: http://127.0.0.1:8080/
card:
	python3 mock_card.py 8080

clean:
	rm -rf $(OUT)

.PHONY: test ui card clean
