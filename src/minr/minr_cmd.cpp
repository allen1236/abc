#include "base/abc/abc.h"
#include "base/main/main.h"
#include "base/main/mainInt.h"
#include "aig/gia/gia.h"

static int Minr_CommandAbc9Minr(Abc_Frame_t* pAbc, int argc, char** argv);

void init(Abc_Frame_t* pAbc) {
  Cmd_CommandAdd(pAbc, "MINR", "&minr", Minr_CommandAbc9Minr , 0);
  
}

void destroy(Abc_Frame_t *pAbc) {}

Abc_FrameInitializer_t frame_initializer = {init, destroy};

struct PackageRegistrationManager
{
    PackageRegistrationManager() { Abc_FrameAddInitializer(&frame_initializer); }
} minrPackageRegistrationManager;

int Minr_CommandAbc9Minr(Abc_Frame_t* pAbc, int argc, char** argv) {
  int c;
  Extra_UtilGetoptReset();
  while ((c = Extra_UtilGetopt(argc, argv, "h")) != EOF) {
    switch (c) {
      case 'h':
        goto usage;
      default:
        goto usage;
    }
  }

  if ( pAbc->pGia == NULL )
  {
      Abc_Print( -1, "Minr_CommandAbc9Minr(): There is no AIG.\n" );
      return 0;
  }
  // minimzie reset here
  // TODO

  return 0;

usage:
  Abc_Print(-2, "usage: &minr [-h]\n");
  Abc_Print(-2, "\t        prints the nodes in the network\n");
  Abc_Print(-2, "\t-h    : print the command usage\n");
  return 1;
}
