include $(KOS_BASE)/Makefile.rules

LIBDIR= ../../lib
TERMLIB=$(LIBDIR)/libterminal.a

OBJS += $(shell find . -name '*.c' |sed -e 's,\.\(.*\).c,build\1.o,g')

all: $(TERMLIB)
.DEFAULT: all

build/%.o: %.c Makefile
	@mkdir -p $(shell dirname $@)
	$(KOS_CC) $(KOS_CFLAGS) -I../../include -I../../enDjinn/include -c $< -o $@

$(TERMLIB): $(OBJS)
	@mkdir -p $(LIBDIR)
	$(KOS_AR) rcs $(TERMLIB) $^

clean:
	@rm -rf $(TERMLIB) $(OBJS)
