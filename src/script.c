/* Copyright (c) 2008 Sadrul Habib Chowdhury (sadrul@users.sf.net)
 * 2009 Rui Guo (firemeteor.guo@gmail.com)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program (see the file COPYING); if not, write to the
 * Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02111-1301  USA
 *
 ****************************************************************
 */

#include "config.h"
#include "screen.h"

struct binding *bindings = NULL;

static void
register_binding (struct binding *new_binding)
{
  if (!new_binding->registered)
    {
      new_binding->b_next = bindings;
      bindings = new_binding;
      new_binding->registered = 1;
    }
}

#ifdef LUA_BINDING
extern struct binding lua_binding;
#endif

void LoadBindings(void)
{
#ifdef LUA_BINDING
  register_binding(&lua_binding);
#endif
}

void
FinalizeBindings (void)
{
  struct binding *binding=bindings;
  while(binding)
    {
      if (binding->inited)
        binding->bd_Finit();
      binding = binding->b_next;
    }
}

void 
ScriptSource(int argc, const char **argv)
{
  int ret = 0;
  int async = 0;
  const char *bd_select = 0, *script;
  struct binding *binding = bindings;

  /* Parse the commandline options
   * sourcescript [-async|-a] [-binding|-b <binding>] script
   */
  while (*argv && **argv == '-') {
      // check for (-a | -async)
      if ((*argv[1] == 'a' && !*argv[2])
          || strcmp(*argv, "-async") == 0)
        async = 1;
      // check for (-b | -binding)
      else if ((*argv[1] == 'b' && !*argv[2])
               || strcmp(*argv, "-binding") == 0) {
          argv++;
          bd_select = *argv;
      }
      argv++;
  }
  script = *argv;

  while (binding) {
      if (!bd_select || strcmp(bd_select, binding->name) == 0) {
          //dynamically initialize the binding
          if (!binding->inited)
            binding->bd_Init();

          //and source the script
          if (ret = binding->bd_Source(script, async))
            break;
      }
      binding = binding->b_next;
  }
  if (!ret)
    LMsg(1, "Could not source specified script %s", script);
}

#define ALL_SCRIPTS(fn, params, stop) do { \
  struct binding *iter; \
  for (iter = bindings; iter; iter = iter->b_next) \
    { \
      if (iter->fns->fn && (ret = (iter->fns->fn params)) && stop) \
	break; \
    } \
} while (0)

void ScriptForeWindowChanged(void)
{
  int ret;
  ALL_SCRIPTS(sf_ForeWindowChanged, (), 0);
}

int ScriptProcessCaption(const char *str, struct win *win, int len)
{
  int ret = 0;
  ALL_SCRIPTS(sf_ProcessCaption, (str, win, len), 1);
  return ret;
}

int ScriptCommandExecuted(const char *command, const char **args, int argc)
{
  int ret = 0;
  ALL_SCRIPTS(sf_CommandExecuted, (command, args, argc), 0);
  return ret;
}

