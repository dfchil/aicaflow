include $(KOS_BASE)/Makefile.rules

LIBDIR= ../../../lib
HOSTLIB=$(LIBDIR)/libafx.a

OBJS += $(shell find . -name '*.c' |sed -e 's,\.\(.*\).c,build\1.o,g')

all: $(HOSTLIB)
.DEFAULT: all

build/%.o: %.c Makefile
	@mkdir -p $(shell dirname $@)
	$(KOS_CC) $(KOS_CFLAGS) -I../../../include  -c $< -o $@

$(HOSTLIB): $(OBJS)
	@mkdir -p $(LIBDIR)
	$(KOS_AR) rcs $(HOSTLIB) $^

clean:
	@rm -rf $(HOSTLIB) $(OBJS)
