del .\datalink-A.log
del .\datalink-B.log
gcc -g .\crc32.c .\datalink.c .\datalink.h .\getopt.c .\getopt.h .\lprintf.c .\lprintf.h .\protocol.c .\protocol.h -lws2_32 -o datalink.exe