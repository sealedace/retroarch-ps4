/*  RetroArch - A frontend for libretro.
 *  Copyright (C) 2010-2014 - Hans-Kristian Arntzen
 *  Copyright (C) 2011-2017 - Daniel De Matteis
 *
 *  RetroArch is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  RetroArch is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with RetroArch.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdint.h>
#include <stdlib.h>

#ifdef HAVE_CONFIG_H
#include "../../config.h"
#endif

#include <pad.h>

#include <boolean.h>
#include <libretro.h>
#include <retro_miscellaneous.h>

#include "../input_driver.h"

#ifdef HAVE_MOUSE
#if defined(HAVE_OOSDK)
#include <orbis/Mouse.h>
#else
#include <mouse.h>
#endif
#define MOUSE_MAX_HISTORY 1
#endif

#ifdef HAVE_KEYBOARD
#if defined(HAVE_OOSDK)
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <orbis/libkernel.h>
#include <orbis/UserService.h>
#include "../../verbosity.h"

/* The keyboard functions live in the system's libSceKeyboard module, whose
 * exports include the sceDbgKeyboard* family. There is no libSceDbgKeyboard
 * module to link against, so they are looked up at run time: without a
 * keyboard module the driver simply has no keyboard. */
typedef struct SceDbgKeyboardData
{
   uint64_t timestamp;
   uint32_t intercepted;
   uint32_t reserved0;
   int32_t  connected;
   int32_t  length;
   uint32_t led;
   uint32_t modifierKey;
   uint16_t keyCode[16];
   uint8_t  reserved1[32];
} SceDbgKeyboardData;

typedef int (*ps4_kb_init_t)(void);
typedef int (*ps4_kb_open_t)(int32_t user_id, int32_t type, int32_t index,
      const void *param);
typedef int (*ps4_kb_read_state_t)(int32_t handle, SceDbgKeyboardData *data);
typedef int (*ps4_kb_close_t)(int32_t handle);
typedef int (*ps4_kb_set_privilege_t)(uint8_t privilege);
typedef int (*ps4_kb_get_handle_t)(int32_t user_id, int32_t type, int32_t index);


static ps4_kb_read_state_t ps4_kb_read_state = NULL;

static ps4_kb_close_t      ps4_kb_close      = NULL;
#else
#include <dbg_keyboard.h>
#endif
#include "../input_keymaps.h"
#define PS4_MAX_SCANCODE 0xE7
#define PS4_NUM_MODIFIERS 8 /* modifier bits in modifierKey */
uint8_t modifier_lut[PS4_NUM_MODIFIERS][2] =
{
   { 0xE0, 0x01 }, /* LCTRL */
   { 0xE4, 0x10 }, /* RCTRL */
   { 0xE1, 0x02 }, /* LSHIFT */
   { 0xE5, 0x20 }, /* RSHIFT */
   { 0xE2, 0x04 }, /* LALT */
   { 0xE6, 0x40 }, /* RALT */
   { 0xE3, 0x08 }, /* LGUI */
   { 0xE7, 0x80 }  /* RGUI */
};
#endif

#include "../../defines/ps4_defines.h"

/* TODO/FIXME -
 * fix game focus toggle */

typedef struct ps4_input
{
   const input_device_driver_t *joypad;
#ifdef HAVE_MOUSE
   bool mice_connected;
   int32_t mouse_handle;
   bool mouse_button_left;
   bool mouse_button_right;
   bool mouse_button_middle;
   int32_t mouse_x;
   int32_t mouse_y;
#endif
#ifdef HAVE_KEYBOARD
   bool keyboard_connected;
   int32_t keyboard_handle;
   bool keyboard_state[PS4_MAX_SCANCODE + 1];
#endif
} ps4_input_t;

static void ps4_input_poll(void *data)
{
   ps4_input_t *ps4 = (ps4_input_t*)data;

   if (ps4 && ps4->joypad)
      ps4->joypad->poll();

#ifdef HAVE_MOUSE
   int ret;
   SceMouseData mouse_state;
   ret = sceMouseRead(ps4->mouse_handle, &mouse_state, MOUSE_MAX_HISTORY);
   ps4->mice_connected = mouse_state.connected;
   ps4->mouse_x = 0;
   ps4->mouse_y = 0;
   if (ret > 0 && ps4->mice_connected && !(mouse_state.buttons & SCE_MOUSE_BUTTON_INTERCEPTED))
   {
     ps4->mouse_button_left = mouse_state.buttons & SCE_MOUSE_BUTTON_PRIMARY;
     ps4->mouse_button_right = mouse_state.buttons & SCE_MOUSE_BUTTON_SECONDARY;
     ps4->mouse_button_middle = mouse_state.buttons & SCE_MOUSE_BUTTON_OPTIONAL;
     ps4->mouse_x = mouse_state.xAxis;
     ps4->mouse_y = mouse_state.yAxis;
   }
#endif
#ifdef HAVE_KEYBOARD
   unsigned int i = 0;
   int key_sym = 0;
   unsigned key_code = 0;
   uint16_t mod = 0;
   uint32_t modifiers;
   bool key_held = false;
   SceDbgKeyboardData keyboard_state;
#if defined(HAVE_OOSDK)
   memset(&keyboard_state, 0, sizeof(keyboard_state));
   if (ps4->keyboard_handle >= 0 && ps4_kb_read_state)
      ps4_kb_read_state(ps4->keyboard_handle, &keyboard_state);
#else
   sceDbgKeyboardReadState(ps4->keyboard_handle, &keyboard_state);
#endif
   ps4->keyboard_connected = keyboard_state.connected;
   {
     /* Keys held now: the modifier bits, plus the usages listed in
      * keyCode[]. The list is not positional - it reorders as keys are
      * pressed and released - so it is compared as a set. */
     bool held[PS4_MAX_SCANCODE + 1];
     memset(held, 0, sizeof(held));

     if (ps4->keyboard_connected)
     {
        modifiers = keyboard_state.modifierKey;
        for (i = 0; i < PS4_NUM_MODIFIERS; i++)
           if (modifiers & modifier_lut[i][1])
              held[modifier_lut[i][0]] = true;

        for (i = 0; i < sizeof(keyboard_state.keyCode)
              / sizeof(keyboard_state.keyCode[0]); i++)
        {
           key_sym = keyboard_state.keyCode[i];
           /* 0x01-0x03 are the HID error/rollover codes */
           if (key_sym >= 0x04 && key_sym < 0xE0)
              held[key_sym] = true;
        }
     }

     mod = (uint16_t)(ps4->keyboard_connected ? keyboard_state.modifierKey : 0);
     for (key_sym = 1; key_sym <= PS4_MAX_SCANCODE; key_sym++)
     {
        key_held = held[key_sym];
        if (key_held == ps4->keyboard_state[key_sym])
           continue;
        ps4->keyboard_state[key_sym] = key_held;
        key_code = input_keymaps_translate_keysym_to_rk(key_sym);
        input_keyboard_event(key_held, key_code, 0, mod, RETRO_DEVICE_KEYBOARD);
     }
   }
#endif
}

#ifdef HAVE_MOUSE
static int16_t ps4_input_mouse_state(ps4_input_t *ps4, unsigned id, bool screen)
{
   int16_t val = 0;

   if (!ps4->mice_connected)
      return 0;

   switch (id)
   {
      case RETRO_DEVICE_ID_MOUSE_LEFT:
         val = ps4->mouse_button_left;
         break;
      case RETRO_DEVICE_ID_MOUSE_RIGHT:
         val = ps4->mouse_button_right;
         break;
      case RETRO_DEVICE_ID_MOUSE_MIDDLE:
         val = ps4->mouse_button_middle;
         break;
      case RETRO_DEVICE_ID_MOUSE_X:
         val = ps4->mouse_x;
         break;
      case RETRO_DEVICE_ID_MOUSE_Y:
         val = ps4->mouse_y;
         break;
   }

   return val;
}
#endif

#ifdef HAVE_KEYBOARD
/* Whether the key bound to a RetroPad button is held */
static bool ps4_keyboard_bind_pressed(ps4_input_t *ps4, enum retro_key key)
{
   unsigned sym;
   if (key == RETROK_UNKNOWN || key >= RETROK_LAST)
      return false;
   sym = (unsigned)rarch_keysym_lut[key];
   return sym && sym <= PS4_MAX_SCANCODE && ps4->keyboard_state[sym];
}
#endif

static int16_t ps4_input_state(void *data,
      rarch_joypad_info_t *joypad_info,
      const struct retro_keybind **binds,
      unsigned port, unsigned device,
      unsigned idx, unsigned id)
{
   ps4_input_t *ps4           = (ps4_input_t*)data;

   switch (device)
   {
      case RETRO_DEVICE_JOYPAD:
         if (id == RETRO_DEVICE_ID_JOYPAD_MASK)
         {
            unsigned i;
            int16_t ret = 0;
            for (i = 0; i < RARCH_FIRST_CUSTOM_BIND; i++)
            {
               /* Auto-binds are per joypad, not per user. */
               const uint64_t joykey  = (binds[port][i].joykey != NO_BTN)
                  ? binds[port][i].joykey : joypad_info->auto_binds[i].joykey;
               const uint32_t joyaxis = (binds[port][i].joyaxis != AXIS_NONE)
                  ? binds[port][i].joyaxis : joypad_info->auto_binds[i].joyaxis;

               if ((uint16_t)joykey != NO_BTN && ps4->joypad->button(
                        joypad_info->joy_idx, (uint16_t)joykey))
               {
                  ret |= (1 << i);
                  continue;
               }
#ifdef HAVE_KEYBOARD
               if (binds[port][i].valid
                     && ps4_keyboard_bind_pressed(ps4, binds[port][i].key))
               {
                  ret |= (1 << i);
                  continue;
               }
#endif
               if (((float)abs(ps4->joypad->axis(joypad_info->joy_idx, joyaxis)) / 0x8000) > joypad_info->axis_threshold)
               {
                  ret |= (1 << i);
                  continue;
               }
            }

            return ret;
         }
         else
         {
            /* Auto-binds are per joypad, not per user. */
            const uint64_t joykey  = (binds[port][id].joykey != NO_BTN)
               ? binds[port][id].joykey : joypad_info->auto_binds[id].joykey;
            const uint32_t joyaxis = (binds[port][id].joyaxis != AXIS_NONE)
               ? binds[port][id].joyaxis : joypad_info->auto_binds[id].joyaxis;

            if ((uint16_t)joykey != NO_BTN && ps4->joypad->button(
                     joypad_info->joy_idx, (uint16_t)joykey))
               return true;
#ifdef HAVE_KEYBOARD
            if (id < RARCH_BIND_LIST_END && binds[port][id].valid
                  && ps4_keyboard_bind_pressed(ps4, binds[port][id].key))
               return true;
#endif
            if (((float)abs(ps4->joypad->axis(joypad_info->joy_idx, joyaxis)) / 0x8000) > joypad_info->axis_threshold)
               return true;
         }
         break;
      case RETRO_DEVICE_ANALOG:
         if (binds[port])
            return input_joypad_analog(ps4->joypad, joypad_info, port, idx, id, binds[port]);
         break;
#ifdef HAVE_KEYBOARD
      case RETRO_DEVICE_KEYBOARD:
         return ((id < RETROK_LAST) && ps4->keyboard_state[rarch_keysym_lut[(enum retro_key)id]]);
         break;
#endif
#ifdef HAVE_MOUSE
      case RETRO_DEVICE_MOUSE:
         return ps4_input_mouse_state(ps4, id, false);
         break;
      case RARCH_DEVICE_MOUSE_SCREEN:
         return ps4_input_mouse_state(ps4, id, true);
         break;
#endif
   }

   return 0;
}

static void ps4_input_free_input(void *data)
{
   ps4_input_t *ps4 = (ps4_input_t*)data;

   if (ps4 && ps4->joypad)
      ps4->joypad->destroy();

#if defined(HAVE_MOUSE)
   sceMouseClose(ps4->mouse_handle);
#endif
#if defined(HAVE_KEYBOARD)
#if defined(HAVE_OOSDK)
   /* Kept open for the life of the process, see ps4_keyboard_open() */
#else
   sceDbgKeyboardClose(ps4->keyboard_handle);
#endif
#endif
   free(data);
}

#if defined(HAVE_KEYBOARD) && defined(HAVE_OOSDK)
static void *ps4_keyboard_sym(int32_t module, const char *debug_name,
      const char *name, bool *is_debug)
{
   void *sym = NULL;
   if (sceKernelDlsym(module, debug_name, &sym) == 0 && sym)
      *is_debug = true;
   else if (sceKernelDlsym(module, name, &sym) != 0)
      sym = NULL;
   return sym;
}

static void ps4_keyboard_logf(const char *fmt, ...)
{
   /* Straight to the kernel log: RetroArch's own log is switched off
    * once content auto-starts, and the input driver is set up again then. */
   char line[160];
   va_list ap;
   va_start(ap, fmt);
   vsnprintf(line, sizeof(line), fmt, ap);
   va_end(ap);
   sceKernelDebugOutText(0, line);
}

/* The keyboard is opened once per process: the input driver is torn down
 * and set up again around content loading, and a second open of the same
 * port fails. Returns the handle, or a negative value without a keyboard. */
static int32_t ps4_keyboard_open(int32_t user_id)
{
   static int32_t handle            = -1;
   static bool tried                = false;
   char path[256];
   bool is_debug                    = false;
   ps4_kb_init_t kb_init            = NULL;
   ps4_kb_open_t kb_open            = NULL;
   ps4_kb_set_privilege_t kb_priv   = NULL;
   ps4_kb_get_handle_t kb_get       = NULL;
   const char *sandbox              = sceKernelGetFsSandboxRandomWord();
   int32_t module;

   if (tried)
      return handle;
   tried = true;

   snprintf(path, sizeof(path), "/%s/common/lib/libSceKeyboard.sprx",
         sandbox ? sandbox : "system");
   module = (int32_t)sceKernelLoadStartModule(path, 0, NULL, 0, NULL, NULL);
   ps4_keyboard_logf("[Keyboard]: load libSceKeyboard -> 0x%08X\n", (uint32_t)module);
   if (module < 0)
      return -1;

   kb_init           = (ps4_kb_init_t)ps4_keyboard_sym(module,
         "sceDbgKeyboardInit", "sceKeyboardInit", &is_debug);
   kb_open           = (ps4_kb_open_t)ps4_keyboard_sym(module,
         "sceDbgKeyboardOpen", "sceKeyboardOpen", &is_debug);
   ps4_kb_read_state = (ps4_kb_read_state_t)ps4_keyboard_sym(module,
         "sceDbgKeyboardReadState", "sceKeyboardReadState", &is_debug);
   ps4_kb_close      = (ps4_kb_close_t)ps4_keyboard_sym(module,
         "sceDbgKeyboardClose", "sceKeyboardClose", &is_debug);
   if (sceKernelDlsym(module, "sceKeyboardSetProcessPrivilege",
            (void**)&kb_priv) != 0)
      kb_priv = NULL;
   if (sceKernelDlsym(module, "sceKeyboardGetHandle",
            (void**)&kb_get) != 0)
      kb_get = NULL;

   ps4_keyboard_logf("[Keyboard]: %s API, init %d open %d read %d\n",
         is_debug ? "debug" : "retail", kb_init != NULL, kb_open != NULL,
         ps4_kb_read_state != NULL);
   if (!kb_init || !kb_open || !ps4_kb_read_state)
      return -1;

   ps4_keyboard_logf("[Keyboard]: init -> 0x%08X\n", (uint32_t)kb_init());
   if (kb_priv)
      ps4_keyboard_logf("[Keyboard]: set privilege -> 0x%08X\n",
            (uint32_t)kb_priv(1));
   handle = kb_open(user_id, 0 /* standard port */, 0, NULL);
   ps4_keyboard_logf("[Keyboard]: open user 0x%x -> 0x%08X\n", user_id,
         (uint32_t)handle);
   if (handle < 0 && kb_get)
   {
      handle = kb_get(user_id, 0, 0);
      ps4_keyboard_logf("[Keyboard]: get handle -> 0x%08X\n", (uint32_t)handle);
   }
   return handle;
}
#endif

static void* ps4_input_initialize(const char *joypad_driver)
{
   ps4_input_t *ps4 = (ps4_input_t*)calloc(1, sizeof(*ps4));
   if (!ps4)
      return NULL;

   ps4->joypad = input_joypad_init_driver(joypad_driver, ps4);

#if defined(HAVE_MOUSE) || defined(HAVE_KEYBOARD)
  SceUserServiceUserId user_id;
  sceUserServiceGetInitialUser(&user_id);
#ifdef HAVE_MOUSE
  sceMouseInit();
  SceMouseOpenParam param;
  param.behaviorFlag = 0;
  param.behaviorFlag |= SCE_MOUSE_OPEN_PARAM_MERGED;
  ps4->mouse_handle = sceMouseOpen(user_id, SCE_MOUSE_PORT_TYPE_STANDARD, 0, &param);
  ps4->mouse_x = 0;
  ps4->mouse_y = 0;
#endif
#ifdef HAVE_KEYBOARD
#if defined(HAVE_OOSDK)
  ps4->keyboard_handle = ps4_keyboard_open(user_id);
#else
  sceDbgKeyboardInit();
  ps4->keyboard_handle = sceDbgKeyboardOpen(user_id, SCE_DBG_KEYBOARD_PORT_TYPE_STANDARD, 0, NULL);
#endif
  input_keymaps_init_keyboard_lut(rarch_key_map_ps4);
  unsigned int i;
  for (i = 0; i <= PS4_MAX_SCANCODE; i++)
  {
    ps4->keyboard_state[i] = false;
  }
#endif
#endif

   return ps4;
}

static uint64_t ps4_input_get_capabilities(void *data)
{
   (void)data;
   return
#ifdef HAVE_KEYBOARD
      (1 << RETRO_DEVICE_KEYBOARD) |
#endif
#ifdef HAVE_MOUSE
      (1 << RETRO_DEVICE_MOUSE)    |
#endif
      (1 << RETRO_DEVICE_JOYPAD)   |
      (1 << RETRO_DEVICE_ANALOG);
}

static const input_device_driver_t *ps4_input_get_joypad_driver(void *data)
{
   ps4_input_t *ps4 = (ps4_input_t*)data;
   if (ps4)
      return ps4->joypad;
   return NULL;
}

static void ps4_input_grab_mouse(void *data, bool state)
{
   (void)data;
   (void)state;
}

static bool ps4_input_set_rumble(void *data, unsigned port,
      enum retro_rumble_effect effect, uint16_t strength)
{
   ps4_input_t *ps4 = (ps4_input_t*)data;

   if (ps4 && ps4->joypad)
      return input_joypad_set_rumble(ps4->joypad,
         port, effect, strength);
   return false;
}

input_driver_t input_ps4 = {
   ps4_input_initialize,
   ps4_input_poll,
   ps4_input_state,
   ps4_input_free_input,
   NULL,
   NULL,
   ps4_input_get_capabilities,
   "ps4",
   ps4_input_grab_mouse,
   NULL,
   ps4_input_set_rumble,
   ps4_input_get_joypad_driver,
   NULL,
   false
};
