prefix = /usr

all: wp360-fan-ctrl wp360-fan-ctrl-debug

wp360-fan-ctrl: main.c
	$(CC) main.c -l gpiod -l pthread -l rt -o wp360-fan-ctrl

wp360-fan-ctrl-debug: main.c
	$(CC) -DDEBUG main.c -l gpiod -l pthread -l rt -o wp360-fan-ctrl-debug

install: all
	install -d $(DESTDIR)$(prefix)/bin
	install -d $(DESTDIR)$(prefix)/lib/systemd/system
	install -d $(DESTDIR)$(prefix)/lib/wp360-fan-ctrl
	install -d $(DESTDIR)/var/log/wp360-fan-ctrl
	install wp360-fan-ctrl $(DESTDIR)$(prefix)/bin
	install wp360-fan-ctrl-debug $(DESTDIR)$(prefix)/lib/wp360-fan-ctrl
	install wp360-fan-ctrl.service $(DESTDIR)$(prefix)/lib/systemd/system

clean:
	-rm wp360-fan-ctrl wp360-fan-ctrl-debug

.PHONY: all install clean
