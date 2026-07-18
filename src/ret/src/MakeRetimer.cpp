
#include "ret/MakeRetimer.h"

#include "tcl.h"
#include "utl/decode.h"

extern "C" {
extern int Ret_Init(Tcl_Interp* interp);
}

namespace ret {

extern const char* ret_tcl_inits[];

void initRetimer(Tcl_Interp* tcl_interp)
{
    // Define swig TCL commands.
    Ret_Init(tcl_interp);
    // Eval encoded TCL sources.
    utl::evalTclInit(tcl_interp, ret_tcl_inits);
}

} // namespace ret