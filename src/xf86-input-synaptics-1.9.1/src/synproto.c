/*
 * Copyright � 2012 Canonical, Ltd.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include "synproto.h"
#include "synapticsstr.h"


struct SynapticsHwState *
SynapticsHwStateAlloc(SynapticsPrivate * priv)
{
    struct SynapticsHwState *hw;

    hw = calloc(1, sizeof(struct SynapticsHwState));
    if (!hw)
        return NULL;

    return hw;
}

void
SynapticsHwStateFree(struct SynapticsHwState **hw)
{
    free(*hw);
    *hw = NULL;
}

void
SynapticsCopyHwState(struct SynapticsHwState *dst,
                     const struct SynapticsHwState *src)
{
    memcpy(dst, src, sizeof(struct SynapticsHwState));
}

void
SynapticsResetHwState(struct SynapticsHwState *hw)
{
    memset(hw, 0, sizeof (struct SynapticsHwState));
    for(int f=0; f<5;f++)
    {
    	hw->x[f]=-1;
    	hw->y[f]=-1;
    	hw->z[f]=-1;
    }
}

