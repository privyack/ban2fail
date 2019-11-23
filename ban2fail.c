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
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "ban2fail.h"
#include "cntry.h"
#include "ez_dirent.h"
#include "ez_gzfile.h"
#include "ez_stdio.h"
#include "ez_stdlib.h"
#include "iptables.h"
#include "logEntry.h"
#include "logFile.h"
#include "logType.h"
#include "map.h"
#include "maxoff.h"
#include "str.h"
#include "util.h"

enum {
   BLOCKED_FLG          =1<<0,
   WOULD_BLOCK_FLG      =1<<1,
   UNJUST_BLOCK_FLG     =1<<2,
   WHITELIST_FLG        =1<<3
};

/*==================================================================*/
/*=================== Support structs ==============================*/
/*==================================================================*/
struct cntryStat {
   char *cntry;
   unsigned count;
};

/* Need this for initialization from configuration file */
struct initInfo {
  const char *symStr;
  int (*init_f)(CFGMAP *map, char *symStr);
};

/*==================================================================*/
/*================= Forward declarations ===========================*/
/*==================================================================*/

static int cntryStat_count_qsort(const void *p1, const void *p2);
static int configure(CFGMAP *h_cfgmap, const char *pfix);
static int logentry_count_qsort(const void *p1, const void *p2);
static int map_byCountries(LOGENTRY *e, MAP *h_map);
static int stub_init(CFGMAP *map, char *symStr);
static int whitelist_init(CFGMAP *h_cfgmap, char *symStr);


/*==================================================================*/
/*========================= static data ============================*/
/*==================================================================*/
struct Global G= {
   .cacheDir= CACHEDIR,
   .lockPath= LOCKPATH,

   .version= {
      .major= 0,
      .minor= 9,
      .patch= 4
   }
};

const static struct initInfo S_initInfo_arr[] = {
   {.symStr= "MAX_OFFENSES",     .init_f= MAXOFF_init},
   {.symStr= "LOGTYPE",          .init_f= LOGTYPE_init},
   {/* Terminating member */}
};


static const struct bitTuple BlockBitTuples[]= {
   {.name= "BLOCKED", .bit= BLOCKED_FLG},
   {.name= "+WouldBLOCK+", .bit= WOULD_BLOCK_FLG},
   {.name= "-UnjustBLOCK-", .bit= UNJUST_BLOCK_FLG},
   {.name= "Whitelisted",   .bit= WHITELIST_FLG},
   {/* Terminating member */}
};

/*================ Local only static struct  ======================*/
static struct {

   MAP addr_map;

   CFGMAP cfgmap;

   PTRVEC toBlock_vec,
          toUnblock_vec;

} S;

/*==================================================================*/
/*======================== main() ==================================*/
/*==================================================================*/

/* Enums for long options */
enum {
   VERSION_OPT_ENUM=128, /* Larger than any printable character */
   HELP_OPT_ENUM
};

int
main(int argc, char **argv)
/***************************************************************
 * Program execution begins here.
 */
{
   int rtn= EXIT_FAILURE,
       lock_fd= -1;

   char *confFile= CONFIGFILE;

   /* Prepare static data */
   // global
   MAP_constructor(&G.logType_map, 10, 10);

   // local
   MAP_constructor(&S.addr_map, 1000, 200);

   PTRVEC_constructor(&S.toBlock_vec, 100000);
   PTRVEC_constructor(&S.toUnblock_vec, 100000);

   { /*=== Parse command line arguments ===*/
      int c, errflg= 0;
      extern char *optarg;
      extern int optind, optopt;

      for(;;) {

         static const struct option long_options[]= {
            {"help", no_argument, 0, HELP_OPT_ENUM},
            {"version", no_argument, 0, VERSION_OPT_ENUM},
            {}
         };

         int c, option_ndx= 0;

         c= getopt_long(argc, argv, ":act:v", long_options, &option_ndx);

         if(-1 == c) break;

        switch(c) {

            /* print usage help */
            case HELP_OPT_ENUM:
               ++errflg;
               break;

            case 'c':
               G.flags |= GLB_LIST_CNTRY_FLG;
               break;

            case 'a':
               G.flags |= GLB_LIST_ADDR_FLG;
               break;

            case 't':
               G.flags |= GLB_DONT_IPTABLE_FLG;
               G.cacheDir= CACHEDIR "-test";
               G.lockPath= LOCKPATH "-test";
               confFile= optarg;
               break;

            case 'v':
               G.flags |= GLB_VERBOSE_FLG;
               break;

            case VERSION_OPT_ENUM:
               ez_fprintf(stderr, "ban2fail v%d.%d.%d\n", G.version.major, G.version.minor, G.version.patch);
               return 0;

            case '?':
               ez_fprintf(stderr, "Unrecognized option: -%c\n", optopt);
               ++errflg;
               break;
         }
      }

      if(errflg) {
         ez_fprintf(stderr, 
"Usage:\n"
"%s [options] [-t confFile]\n"
"  --help\tprint this usage message.\n"
"  -a\t\tList results by Address\n"
"  -c\t\tlist results by Country\n"
"  -t confFile\tTest confFile, do not apply iptables rules\n"
"  --version\tprint the version number and exit.\n"
         , argv[0]
         );

         goto abort;
      }
   } /* Done with command line arguments */

   /* Make sure we will be able to run iptables */
   if(getuid()) {
      eprintf("ERROR: You must be root to run iptables!");
      goto abort;
   }

   { /*============== Read the configuration file ==============*/
      if(!CFGMAP_file_constructor(&S.cfgmap, confFile)) {
         eprintf("ERROR: failed to read configuration from \"%s\"", confFile);
         goto abort;
      }

      /* Just leave the S.cfgmap in place, so all the value strings
       * don't need to be copied.
       */

   }

   { /*============== Obtain a lock on our lockfile ==============*/
      /* Make sure the file exists by open()'ing */
      lock_fd= open(G.lockPath, O_CREAT|O_WRONLY|O_CLOEXEC, 0640);
      if(-1 == lock_fd) {
         sys_eprintf("ERROR: open(\"%s\") failed");
         goto abort;
      }

      /* Let's get a exclusive lock */
      int rc= flock(lock_fd, LOCK_EX|LOCK_NB);
      if(-1 == rc) {
         sys_eprintf("ERROR: flock(\"%s\") failed", G.lockPath);
         goto abort;
      }
   }


   { /*============== Open our cache, instantiate LOGTYPE objects ==============*/

      /* Make the directory if needed */
      if(access(G.cacheDir, F_OK)) {
         /* errno will be set if access() fails */
         errno= 0;

         ez_mkdir(G.cacheDir, 0700);
      }

      if(G.flags & GLB_PRINT_MASK) {
         ez_fprintf(stdout, "=============== ban2fail v%d.%d.%d =============\n"
               , G.version.major
               , G.version.minor
               , G.version.patch
               );
         fflush(stdout);
      }

      { /*============== Implement configuration ==============*/

         if(configure(&S.cfgmap, NULL)) {
            eprintf("ERROR: failed to realize configuration in \"%s\"", confFile);
            goto abort;
         }

         if(G.flags & GLB_VERBOSE_FLG) { /* Warn about unused symbols */
            CFGMAP_print_unused_symbols(&S.cfgmap, stdout);
            fflush(stdout);
         }

         /* Just leave the S.cfgmap in place, so all the value strings
          * don't need to be copied.
          */

         /* We're done with disk I/O, so release lock */
         flock(lock_fd, LOCK_UN);
         ez_close(lock_fd);
         lock_fd= -1;
      }

      { /* Check cache for logType directories not in our current map */
         DIR *dir= ez_opendir(G.cacheDir);
         struct dirent *entry;

         while((entry= ez_readdir(dir))) {

            /* Skip uninteresting entries */
            if('.' == *entry->d_name) continue;

            LOGTYPE *t= MAP_findStrItem(&G.logType_map, entry->d_name);
            /* If there is a matching entry, then do not delete results */
            if(t)
               continue;

            /* Make the path with filename */
            static char pathBuf[PATH_MAX];
            snprintf(pathBuf, sizeof(pathBuf), "%s/%s", G.cacheDir, entry->d_name);

            /* Remove unused directory & contents. */
            ez_rmdir_recursive(pathBuf);

         }
         ez_closedir(dir);
      }

      unsigned nFound= 0;
      MAP_visitAllEntries(&G.logType_map, (int(*)(void*,void*))LOGTYPE_offenseCount, &nFound);

      if(G.flags & GLB_PRINT_MASK) {
         ez_fprintf(stdout, "===== Found %u total offenses =====\n", nFound);
         fflush(stdout);
      }
   }

   { /******* Now get a map of LOGENTRY objects that have combined counts ****/

      /* List by address. Make a addr_map of LOGENTRY objects with composite counts */
      MAP_visitAllEntries(&G.logType_map, (int(*)(void*,void*))LOGTYPE_map_addr, &S.addr_map);
      unsigned nItems= MAP_numItems(&S.addr_map);

      {
         LOGENTRY *leArr[nItems];
         MAP_fetchAllItems(&S.addr_map, (void**)leArr);
         qsort(leArr, nItems, sizeof(LOGENTRY*), logentry_count_qsort);

         /* Process each LOGENTRY item */
         for(unsigned i= 0; i < nItems; ++i) {
            int flags=0;

            LOGENTRY *e= leArr[i];

            if(IPTABLES_is_currently_blocked(e->addr))
               flags |= BLOCKED_FLG;

             int nAllowed= MAXOFF_allowed(e->addr);

             if(-1 == nAllowed)
                flags |= WHITELIST_FLG;

             if((-1 == nAllowed || e->count <= nAllowed) &&
                (flags & BLOCKED_FLG)) {

                  flags |= UNJUST_BLOCK_FLG;
                  PTRVEC_addTail(&S.toUnblock_vec, e->addr);
             }

             if(!(flags & BLOCKED_FLG) &&
                -1 != nAllowed &&
                e->count > nAllowed)
             {

                  flags |= WOULD_BLOCK_FLG;
                  PTRVEC_addTail(&S.toBlock_vec, e->addr);
             }

            /* Print out only for list option */
            if(G.flags & GLB_LIST_ADDR_FLG) {

               ez_fprintf(stdout, "%-15s: %5u offenses %s (%s)\n"
                     , e->addr
                     , e->count
                     , e->cntry[0] ? e->cntry : "--"
                     , bits2str(flags, BlockBitTuples)
                     );
            }

         } /*--- End of LOGENTRY processing ---*/

         /* Take care of summary blocking and reporting */
         unsigned n2Block= PTRVEC_numItems(&S.toBlock_vec);
         unsigned n2Unblock= PTRVEC_numItems(&S.toUnblock_vec);

         if(!(G.flags & GLB_DONT_IPTABLE_FLG)) {

            if(n2Block) {

               if(IPTABLES_block_addresses(&S.toBlock_vec, 10)) {
                  eprintf("ERROR: cannot block addresses!");
                  goto abort;
               }
               printf("Blocked %u new hosts\n", n2Block);
            }

            if(n2Unblock) {

               if(IPTABLES_unblock_addresses(&S.toUnblock_vec, 10)) {
                  eprintf("ERROR: cannot unblock addresses!");
                  goto abort;
               }
               printf("Unblocked %u hosts\n", n2Unblock);
            }

         } else {

            if(n2Block) 
               printf("Would block %u new hosts\n", n2Block);

            if(n2Unblock)
               printf("Would unblock %u new hosts\n", n2Unblock);
         }


         /* List offenses by country if directed to do so */
         if(G.flags & GLB_LIST_CNTRY_FLG) {

            /* Map for indexing cntryStat objects */
            static MAP byCntry_map;
            MAP_sinit(&byCntry_map, 100, 100);

            /* Build index by trawling existing by-address map */
            MAP_visitAllEntries(&S.addr_map, (int(*)(void*,void*))map_byCountries, &byCntry_map);

            /* Now get all cntStat handles in a vector */
            unsigned vec_sz= MAP_numItems(&byCntry_map);
            struct cntryStat *rtn_vec[vec_sz];

            MAP_fetchAllItems(&byCntry_map, (void**)rtn_vec);

            /* Sort high to low */
            qsort(rtn_vec, vec_sz, sizeof(struct cntryStat*), cntryStat_count_qsort);

            /* Print results */
            for(unsigned i= 0; i < vec_sz; ++i) {

               struct cntryStat *cs= rtn_vec[i];
               ez_fprintf(stdout, "%2s  %5u offenses\n"
                     , cs->cntry[0] ? cs->cntry : "--"
                     , cs->count
                     );
            }
         }
      }
   }

   rtn= EXIT_SUCCESS;
abort:

   /* Make sure lock file is unlocked */
   if(-1 != lock_fd) {
      flock(lock_fd, LOCK_UN);
      ez_close(lock_fd);
   }
   return rtn;
}


/*==================================================================*/
/*============== Supporting functions ==============================*/
/*==================================================================*/

static int
logentry_count_qsort(const void *p1, const void *p2)
/***************************************************************
 * qsort functor puts large counts on top.
 */
{
   const LOGENTRY *le1= *(const LOGENTRY *const*)p1,
                  *le2= *(const LOGENTRY *const*)p2;

   if(le1->count > le2->count) return -1;
   if(le1->count < le2->count) return 1;
   return 0;
}

static int
cntryStat_count_qsort(const void *p1, const void *p2)
/***************************************************************
 * qsort functor puts large counts on top.
 */
{
   const struct cntryStat
      *cs1= *(const struct cntryStat *const*)p1,
      *cs2= *(const struct cntryStat *const*)p2;

   if(cs1->count > cs2->count) return -1;
   if(cs1->count < cs2->count) return 1;
   return 0;
}

static int
configure(CFGMAP *h_cfgmap, const char *pfix)
/*****************************************************************
 * dynamic initialization from contents of configuration
 * dictionary.
 */
{
  int rtn= 1;
  const CFGMAP_ENTRY *pCde;
  const struct initInfo *pIi;

  for(pIi= S_initInfo_arr; pIi->symStr; ++pIi) {
    char buf[1024];
    /* Create the symbol we will look for */
    snprintf(buf, sizeof(buf), "%s\\%s", pfix ? pfix : "", pIi->symStr);

    if((pCde= CFGMAP_find(h_cfgmap, buf))) {
      unsigned i;
      for(i= 0; i < CFGMAP_ENTRY_numValues(pCde); ++i) {
        /* Create the name for this object */
        snprintf(buf, sizeof(buf), "%s\\%s", pfix ? pfix : "", CFGMAP_ENTRY_value(pCde, i));
        /* Call the initialization function */
        if((*pIi->init_f)(h_cfgmap, buf)) goto abort;
        /* recurse with longer pfix */
        if(configure(h_cfgmap, buf)) {
          eprintf("ERROR: initialization function failed.");
          goto abort;
        }
      }
    }
  }

  rtn= 0;

abort:
  return rtn;
}

#ifdef DEBUG
static int
stub_init(CFGMAP *map, char *symStr)
/*****************************************************************
 * Stand-in xxx_init() function until a proper one is implemented.
 */
{
   eprintf("HERE, symStr= \"%s\"", symStr);
   return 0;
}
#endif

static int
map_byCountries(LOGENTRY *e, MAP *h_map)
/**************************************************************
 * Generate a "by country" map of cntryStat objects.
 */
{
   struct cntryStat *cs= MAP_findStrItem(h_map, e->cntry);
   if(!cs) {
      cs= calloc(1, sizeof(*cs));
      cs->cntry= e->cntry;
      MAP_addStrKey(h_map, cs->cntry, cs);
   }

   cs->count += e->count;

   return 0;
}

