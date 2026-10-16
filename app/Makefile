APP = acap_video_encoder

PKGS = fcgi
CFLAGS += $(shell pkg-config --cflags $(PKGS)) -Wall -Wextra -Werror -Wno-format-truncation -O2
LDLIBS += $(shell pkg-config --libs $(PKGS))

all: $(APP)

$(APP): encoder.c
	$(CC) $(CFLAGS) $< $(LDFLAGS) $(LDLIBS) -o $@
	$(STRIP) $@

clean:
	rm -f $(APP) *.o *.eap *_LICENSE.txt package.conf* param.conf

.PHONY: all clean
