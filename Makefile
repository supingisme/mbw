#CFLAGS=-O2 -Wall -g
NAME=mbw
TARFILE=${NAME}.tar.gz

CC=aarch64-unknown-nto-qnx7.0.0-gcc

qnx:
	$(CC) $(CFLAGS)  util.c  mbw.c -o mbw 

clean:
	rm -f mbw
	rm -f ${NAME}.tar.gz

${TARFILE}: clean
	 tar cCzf .. ${NAME}.tar.gz --exclude-vcs ${NAME} || true

rpm: ${TARFILE}
	 rpmbuild -ta ${NAME}.tar.gz 
