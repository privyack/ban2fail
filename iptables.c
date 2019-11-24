/***************************************************************************
 *   Copyright (C) 2019 by John D. Robertson                               *
 *   john@rrci.com                                                         *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 3 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************/
#include <assert.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include "ban2fail.h"
#include "ez_stdio.h"
#include "iptables.h"
#include "map.h"
#include "util.h"

static struct {

   int is_init;
   MAP addr_map;

} S;

static void
initialize (void)
/********************************************************
 * Prepare static data, populate index from iptables.
 */
{
   S.is_init= 1;

   MAP_constructor(&S.addr_map, 1000, 200);

   static char lbuf[1024];
   static char addr[64];
   FILE *fh= ez_popen(IPTABLES " -nL INPUT 2>/dev/null", "r");
   for(unsigned i= 0; ez_fgets(lbuf, sizeof(lbuf)-1, fh); ++i) {
      if(0 == i || 1 == i) continue;
      if(1 != sscanf(lbuf, "DROP all -- %63s 0.0.0.0/0", addr)) {
         eprintf("ERROR: scanning pattern");
         continue;
      }
      MAP_addStrKey(&S.addr_map, addr, (void*)-1);
   }
   ez_pclose(fh);

   fh= ez_popen(IP6TABLES " -nL INPUT 2>/dev/null", "r");
   for(unsigned i= 0; ez_fgets(lbuf, sizeof(lbuf)-1, fh); ++i) {
      if(0 == i || 1 == i) continue;

// DROP       all      2607:5300:60:653b::  ::/0
      if(1 != sscanf(lbuf, "DROP all %63s ::/0", addr)) {
         eprintf("ERROR: scanning pattern");
         continue;
      }
      MAP_addStrKey(&S.addr_map, addr, (void*)-1);
   }
   ez_pclose(fh);

}

int
IPTABLES_is_currently_blocked(const char *addr)
/********************************************************
 * This provides an efficient lookup of addresses blocked
 * by iptables in the filter table, INPUT chain.
 * 
 * RETURN:
 * 1 if the supplied addr is blocked by iptables.
 * 0 otherwise.
 */
{
   if(!S.is_init)
      initialize();

   /* See if this addr is in the map */
   if(MAP_findStrItem(&S.addr_map, addr)) return 1;
   return 0;
}

static int
addrCmp_pvsort(const void *const* pp1, const void *const* pp2)
/**************************************************************
 * PTRVEC_sort() comparison function for addresses, puts
 * ipv6 at the bottom.
 */
{
   const char *addr1= *((const char*const*)pp1),
              *addr2= *((const char*const*)pp2);

   if(strchr(addr2, ':')) {
      if(!strchr(addr1, ':')) return -1;
   } else {
      if(strchr(addr1, ':')) return 1;
   }
  
   return 0;
}

static int
_control_addresses(int cmdFlag, PTRVEC *h_vec, unsigned batch_sz)
/**************************************************************
 * (Un)block addresses in batches of batch_sz.
 */
{
   if(!S.is_init)
      initialize();

   int rtn= -1;

   /* Sanity check for debugging */
   assert(batch_sz > 0 && batch_sz <= 100);

   /* Use string buffer to form command */
   static STR cmd_sb;

   const char *addr;

   /* Put any ipv6 addresses at end */
   PTRVEC_sort(h_vec, addrCmp_pvsort);

   /* Work through ipv4 addresses in the vector */
   while((addr= PTRVEC_remHead(h_vec)) &&
         !strchr(addr, ':'))
   {
      /* Initialize / reset string buffer */
      STR_sinit(&cmd_sb, 256+batch_sz*42);

      /* Beginning of command string, with first source address */
      STR_sprintf(&cmd_sb, IPTABLES " 2>&1 -%c INPUT -s %s", cmdFlag, addr);

      /* Append additional source addresses */
      unsigned i= 1;
      while(i < batch_sz &&
            (addr= PTRVEC_remHead(h_vec)) &&
            !strchr(addr, ':'))
      {
         /* employ multiple source addresses for batching */
         STR_sprintf(&cmd_sb, ",%s", addr);
         ++i;
      }

      /* Put the end of the command in place */
      STR_sprintf(&cmd_sb, " -j DROP");

      /* Run iptables */
      FILE *fh= ez_popen(STR_str(&cmd_sb), "r");
      /* Display any output from iptables */
      static char lbuf[1024];
      while(ez_fgets(lbuf, sizeof(lbuf), fh))
         ez_fprintf(stderr, "NOTE: iptables output: %s", lbuf);

      /* All done */
      ez_pclose(fh);

      /* If the last address pulled was ipv6, move on */
      if(addr && strchr(addr, ':')) break;
   }

   /* Work through ipv6 addresses in the vector */
   for( ; addr; (addr= PTRVEC_remHead(h_vec))) {

      /* Initialize / reset string buffer */
      STR_sinit(&cmd_sb, 256+batch_sz*42);

      /* Beginning of command string, with first source address */
      STR_sprintf(&cmd_sb, IP6TABLES " 2>&1 -%c INPUT -s %s", cmdFlag, addr);

      /* Append additional source addresses */
      unsigned i= 1;
      while(i < batch_sz && (addr= PTRVEC_remHead(h_vec))) {

         /* employ multiple source addresses for batching */
         STR_sprintf(&cmd_sb, ",%s", addr);
         ++i;
      }

      /* Put the end of the command in place */
      STR_sprintf(&cmd_sb, " -j DROP");

      /* Run iptables */
      FILE *fh= ez_popen(STR_str(&cmd_sb), "r");
      /* Display any output from iptables */
      static char lbuf[1024];
      while(ez_fgets(lbuf, sizeof(lbuf), fh))
         ez_fprintf(stderr, "NOTE: ip6tables output: %s", lbuf);

      /* All done */
      ez_pclose(fh);

   }

   rtn= 0;
abort:
   return rtn;
}

int
IPTABLES_block_addresses(PTRVEC *h_vec, unsigned batch_sz)
/**************************************************************
 * Block addresses in batches of batch_sz.
 */
{
   if(!S.is_init)
      initialize();

   return _control_addresses('A', h_vec, batch_sz);

}

int
IPTABLES_unblock_addresses(PTRVEC *h_vec, unsigned batch_sz)
/**************************************************************
 * Block addresses in batches of batch_sz.
 */
{
   if(!S.is_init)
      initialize();

   return _control_addresses('D', h_vec, batch_sz);

}
