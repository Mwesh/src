#include <stdio.h>
#include <stddef.h>

static struct sss{
  size_t f;
  char * snd;
} sss;

#define _offsetof(st,f) ((char *)&((st *) 16)->f - (char *) 16)

int main (void) {
  printf ("+++Struct size_t-pointer:\n");
  printf ("size=%d,align=%d,offset-size_t=%d,offset-pointer=%d,\nalign-size_t=%d,align-pointer=%d\n",
          sizeof (sss), __alignof__ (sss),
          _offsetof (struct sss, f), _offsetof (struct sss, snd),
          __alignof__ (sss.f), __alignof__ (sss.snd));
  return 0;
}
