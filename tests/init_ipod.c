#include <gpod/itdb.h>
#include <stdio.h>
int main(int c,char**v){GError*e=NULL; if(!itdb_init_ipod(v[1],"MB565","TestPod",&e)){printf("fail %s\n",e->message);return 1;}return 0;}
