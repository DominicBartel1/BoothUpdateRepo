PREFIX = /usr/fcos/bin
CC = gcc
INCS = -I/usr/include/fedm -I/usr/include/json-glib-1.0 -I/usr/include/glib-2.0 -I/usr/lib64/glib-2.0/include -I/usr/include/gio-unix-2.0
LIBS = -lfedm -lstdc++ -L/usr/lib64 -ljson-glib-1.0 -lgio-2.0 -lgobject-2.0 -lglib-2.0 -lmosquitto
CFLAGS = -pthread -std=c++11 -Wall
OBJS = readConsumables.o
EXES = readConsumables

all: $(OBJS) $(EXES)

readConsumables.o: readConsumables.cpp
	$(CC) $(INCS) $(CFLAGS) -c readConsumables.cpp

readConsumables: readConsumables.o
	$(CC) $(LIBS) -Wl,--copy-dt-needed-entries -o readConsumables $(OBJS)

clean:
	rm -f *.o *~ core $(EXES)

install: readConsumables
	install -D readConsumables $(DESTDIR)$(PREFIX)/readConsumables
