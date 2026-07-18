%{
#include "ord/OpenRoad.hh"
#include "ret/Retimer.h"
%}

%include "../../Exception.i"

%inline %{

namespace ret {

void
layout_aware_retime_cmd()
{
    ret::Retimer* retimer = ord::OpenRoad::openRoad()->getRetimer();
    retimer->layoutAwareRetime();
}


} // namespace ret

%} // inline