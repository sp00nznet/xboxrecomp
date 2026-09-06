#ifndef XBOX_D3D8_FVF_H
#define XBOX_D3D8_FVF_H

#include "d3d8_xbox.h"

static inline int d3d8_fvf_transformed(DWORD fvf)
{
    return (fvf & D3DFVF_POSITION_MASK) == D3DFVF_XYZRHW;
}

/* Position is an encoded field, not independent bits. Beta slots precede
 * normals/colors even when the current shader does not consume weights. */
static inline UINT d3d8_fvf_position_bytes(DWORD fvf)
{
    DWORD position = fvf & D3DFVF_POSITION_MASK;
    switch (position) {
    case D3DFVF_XYZ:    return 12;
    case D3DFVF_XYZRHW: return 16;
    case D3DFVF_XYZB1:
    case D3DFVF_XYZB2:
    case D3DFVF_XYZB3:
    case D3DFVF_XYZB4:
    case D3DFVF_XYZB5:
        return 12 + 4 * ((position - D3DFVF_XYZRHW) / 2);
    default: return 0;
    }
}

#endif
