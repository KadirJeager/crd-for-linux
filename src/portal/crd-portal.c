#include "portal.h"

#include <stdio.h>

int
main(int argc, char **argv)
{
  if (argc != 2 || argv[1][0] == '\0') {
    fprintf(stderr, "kullanım: crd-portal <dbus-adresi>\n");
    return 2;
  }
  return crd_portal_run(argv[1]);
}
