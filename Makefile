CFLAGS=--std=gnu99 -Wall -O3

blt_test: blt_test.c blt.c

blt_bm: blt_bm.c blt.c bm.c
	$(CC) $(CFLAGS) -o $@ $^ -ltcmalloc

cbt_bm: cbt_bm.c cbt.c bm.c
	$(CC) $(CFLAGS) -o $@ $^ -ltcmalloc

push:
	git push git@github.com:blynn/blt.git master
	git push https://code.google.com/p/blynn-blt/ master
