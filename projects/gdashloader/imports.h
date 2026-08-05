/* imports.h -- libcocos2dcpp.so + libfmod.so import resolution (Linux)
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __IMPORTS_H__
#define __IMPORTS_H__

#include "so_util.h"

extern DynLibFunction symtable_gdash[];

// Pick real glibc sockets (enabled=true, like the Switch port's net_shim.c)
// or the fail-fast offline stubs (false) for every socket import, before the
// game libs are loaded.
void gdash_network_init(bool enabled);

// resolve the real FMOD createSound/createStream (called after libfmod.so is
// loaded and relocated; the import-table hooks tail into them)
void fmod_hooks_init(so_module *fmod_mod);

#endif
