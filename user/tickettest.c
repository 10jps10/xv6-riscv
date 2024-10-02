#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int
main(int argc, char *argv[])
{
  int tickets = 5;
  settickets(tickets);
  exit(0);
}
