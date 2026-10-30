#ifndef __PIGLET_COMPAT_H__
#define __PIGLET_COMPAT_H__

/* OpenOrbis toolchain (v0.5.x) compatibility for the orbisdev-style
 * <piglet.h> that orbis_ctx.c / orbis_common.h were written against. */

#include <stdint.h>
#include <stdbool.h>
#include <orbis/Pigletv2VSH.h>

typedef OrbisPglConfig ScePglConfig;

#define SCE_PGL_FLAGS_USE_COMPOSITE_EXT   ORBIS_PGL_FLAGS_USE_COMPOSITE_EXT
#define SCE_PGL_FLAGS_USE_FLEXIBLE_MEMORY ORBIS_PGL_FLAGS_USE_FLEXIBLE_MEMORY

/* Same layout as OrbisPglWindow (EGLNativeWindowType) */
typedef struct SceWindow
{
   uint32_t id;
   uint32_t width;
   uint32_t height;
   uint32_t padding;
} SceWindow;

/* Pigletv2VSH.h undefines its struct layout helpers; orbis_common.h
 * needs them for SceShdrCacheConfig. */
#define JOIN_HELPER(x, y) x##y
#define JOIN(x, y) JOIN_HELPER(x, y)
#define TYPE_PAD(size) char JOIN(_pad_, __COUNTER__)[size]
#define TYPE_BEGIN(name, size) name { union { TYPE_PAD(size)
#define TYPE_END(...) }; } __VA_ARGS__
#define TYPE_FIELD(field, offset) struct { TYPE_PAD(offset); field; }

#endif
