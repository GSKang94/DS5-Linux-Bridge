//
// bt_generic.h -- Thin compatibility header for 8BitDo build.
//
// The 8BitDo web API uses bt_get_status() from the shared bt.h. This header
// exists so web_api_8bitdo.cpp can include a name that doesn't imply DS5.
//

#ifndef DS5_BRIDGE_BT_GENERIC_H
#define DS5_BRIDGE_BT_GENERIC_H

#include "bt.h"

#endif // DS5_BRIDGE_BT_GENERIC_H
