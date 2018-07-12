
/*
 *   THIS FILE IS UNDER RCS - DO NOT MODIFY UNLESS YOU HAVE
 *   CHECKED IT OUT USING THE COMMAND CHECKOUT.
 *
 *    $Id: transport.c 6809 2016-09-14 16:14:37Z paulf $
 *
 *    Revision history:
 *     $Log$
 *     Revision 1.6  2005/04/15 17:29:16  dietz
 *     Modified tport_bufthr() to look for termination flag in both the
 *     public and private rings.
 *
 *     Revision 1.5  2004/03/22 21:12:32  kohler
 *     Added: #include <stdlib.h>
 *     tport_bufthr() now returns NULL
 *
 *     Revision 1.4  2001/05/04 23:42:10  dietz
 *     Changed flag arg of tport_putflag from short to int to handle
 *     processids properly.
 *
 *     Revision 1.3  2000/06/02 17:16:22  dietz
 *     Fixed tport_putmsg,tport_copyto to always release semaphore before returning
 *
 *     Revision 1.2  2000/03/25 01:33:28  dietz
 *     added signal blocking to protect semaphore handling code
 *
 *     Revision 1.1  2000/02/14 18:46:17  lucky
 *     Initial revision
 *
 *
 */


  /********************************************************************
   *                                                        3/2000    *
   *                           transport.c                            *
   *                                                                  *
   *   Transport layer functions to access shared memory regions.     *
   *                                                                  *
   *   written by Lynn Dietz, Will Kohler with inspiration from       *
   *       Carl Johnson, Alex Bittenbinder, Barbara Bogaert           *
   *                                                                  *
   ********************************************************************/

#include <sys/types.h> 
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/sem.h>
#include <stropts.h>
#include <string.h>
#include <time.h>
#include <thread.h>
#include <transport.h>

static short Put_Init=1;           /* initialization flag */
static short Get_Init=1;           /* initialization flag */
static short Copyfrom_Init=1;      /* initialization flag */
static short Flag_Init=1;          /* initialization flag */
static SHM_INFO smf_region;
static long shm_flag_key;
#define SHM_FLAG_DEFAULT_KEY 9999
#define SHM_FLAG_RING "FLAG_RING"

#define FF_REMOVE 1
#define FF_FLAG2ADD 2
#define FF_FLAG2DIE 3
#define FF_GETFLAG 4
#define FF_CLASSIFY 5

/* These functions are for internal use by transport functions only
   ****************************************************************/ 
void  tport_syserr  ( char *, long );
void  tport_buferror( short, char * );
void  tport_blocksigs( void );
void  tport_releasesigs( void );
static int tport_doFlagOp( SHM_INFO* region, int pid, int op ); 

/* These statements and variables are required by the functions of 
   the input-buffering thread 
   ***************************************************************/
#include <earthworm.h>  
volatile SHM_INFO *PubRegion;      /* transport public ring      */
volatile SHM_INFO *BufRegion;      /* pointer to private ring    */
volatile MSG_LOGO *Getlogo;        /* array of logos to copy     */
volatile short     Nget;           /* number of logos in getlogo */
volatile unsigned  MaxMsgSize;     /* size of message buffer     */
volatile char     *Message;        /* message buffer             */
static unsigned char MyModuleId;   /* module id of main thread   */
unsigned char      MyInstid;       /* inst id of main thread     */
unsigned char      TypeError;      /* type for error messages    */

/******************** function tport_create *************************/
/*         Create a shared memory region & its semaphore,           */
/*           attach to it and initialize header values.             */  
/********************************************************************/

void tport_create( SHM_INFO *region,   /* info structure for memory region  */
		   long      nbytes,   /* size of shared memory region      */
		   long      memkey )  /* key to shared memory region       */ 
{
   int             regid;     /* shared memory region identifier   */
   int             semid;     /* semaphore identifier              */
   SHM_HEAD       *shm;       /* pointer to start of memory region */
   union semun     semarg;    /* argument for semaphore operations */
   struct shmid_ds shmbuf;    /* shared memory data structure      */
   int             res;

/**** Destroy memory region if it already exists ****/

   regid = shmget( memkey, nbytes, 0 );
   if ( regid != -1 )
   {
      res = shmctl( regid, IPC_RMID, &shmbuf );
      if ( res == -1 )
         tport_syserr( "tport_create shmctl", memkey );
   }

/**** Create shared memory region ****/

   regid = shmget( memkey, nbytes, IPC_CREAT | 0666 );
   if ( regid == -1 )
      tport_syserr( "tport_create shmget", memkey );

/**** Attach to shared memory region ****/

   shm = (SHM_HEAD *) shmat( regid, 0, SHM_RND );
   if ( shm == (SHM_HEAD *) -1 )
      tport_syserr( "tport_create shmat", memkey );

/**** Initialize shared memory region header ****/
   shm->nbytes = nbytes;
   shm->keymax = nbytes - sizeof(SHM_HEAD);
   shm->keyin  = 0;                           /* cosmetic change 980428:ldd */
   shm->keyold = shm->keyin;
   shm->flag = 0;
   
/**** Make semaphore for this shared memory region & set semval = SHM_FREE ****/

   semid = semget( memkey, 1, IPC_CREAT | 0666 );
   if ( semid == -1 )
      tport_syserr( "tport_create semget", memkey );

   semarg.val = SHM_FREE;
   res = semctl( semid, 0, SETVAL, semarg );
   if ( res == -1 )
      tport_syserr( "tport_create semctl", memkey );

/**** set values in the shared memory information structure ****/
   region->addr = shm;
   region->mid  = regid;
   region->sid  = semid;
   region->key  = memkey;
}


/******************** function tport_destroy *************************/
/*                Destroy a shared memory region.                    */
/*********************************************************************/

void tport_destroy( SHM_INFO *region )         
{
   struct shmid_ds shmbuf;    /* shared memory data structure      */
   union semun     semarg;    /* argument for semaphore operations */
   int res;

/***** Detach from shared memory region *****/

   res = shmdt( (char *) region->addr );
   if ( res == -1 )
      tport_syserr( "tport_destroy shmdt", region->key );

/***** Destroy semaphore set for shared memory region *****/

   semarg.val = 0;
   res = semctl( region->sid, 0, IPC_RMID, semarg );
   if ( res == -1 )
      tport_syserr( "tport_destroy semctl", region->key );

/***** Destroy shared memory region *****/

   res = shmctl( region->mid, IPC_RMID, &shmbuf );
   if ( res == -1 )
      tport_syserr( "tport_destroy shmctl", region->key );
}


/******************** function tport_attach *************************/
/*            Map to an existing shared memory region.              */
/********************************************************************/

void tport_attach( SHM_INFO *region,   /* info structure for memory region  */
		   long      memkey )  /* key to shared memory region       */
{
   int        regid;       /* shared memory region identifier   */
   int        semid;       /* semaphore identifier              */
   long       nbytes;      /* size of memory region             */
   SHM_HEAD  *shm;         /* pointer to start of memory region */
   int        res;

/**** attach to header; find out size memory region; detach ****/

   regid = shmget( memkey, sizeof( SHM_HEAD ), 0 );
   if ( regid == -1 ) {
      if ( memkey != shm_flag_key )
         tport_syserr( "tport_attach shmget ->header", memkey );
      /* Must have been called when using an old startstop, so no flag ring */
      smf_region.addr = NULL;
      return;
   }

   shm = (SHM_HEAD *) shmat( regid, 0, SHM_RND );
   if ( shm == (SHM_HEAD *) -1 )
      tport_syserr( "tport_attach shmat ->header", memkey );

   nbytes = shm->nbytes;

   res = shmdt( (char *) shm );
   if ( res == -1 )
      tport_syserr( "tport_attach shmdt ->header", memkey );

/**** reattach to the entire memory region; get semaphore ****/

   regid = shmget( memkey, nbytes, 0 );
   if ( regid == -1 )
      tport_syserr( "tport_attach shmget ->region", memkey );

   shm = (SHM_HEAD *) shmat( regid, 0, SHM_RND );
   if ( shm == (SHM_HEAD *) -1 )
      tport_syserr( "tport_attach shmat ->region", memkey );

   semid = semget( memkey, 1, 0 );
   if ( semid == -1 )
      tport_syserr( "tport_attach semget", memkey );

/**** set values in the shared memory information structure ****/
   region->addr = shm;
   region->mid  = regid;
   region->sid  = semid;
   region->key  = memkey;

   if ( Flag_Init ) {
   	  Flag_Init = 0;
   	  
      shm_flag_key = GetKeyWithDefault( SHM_FLAG_RING, SHM_FLAG_DEFAULT_KEY );
   	  tport_attach( &smf_region, shm_flag_key );
   }
   tport_addToFlag( region, getpid() );
}

/******************** function tport_detach **************************/
/*                Detach from a shared memory region.                */
/*********************************************************************/

void tport_detach( SHM_INFO *region )
{
   int res;

/***** Detach from shared memory region *****/

   res = shmdt( (char *) region->addr );
   if ( res == -1 )
      tport_syserr( "tport_detach shmdt", region->key );
}



/*********************** function tport_putmsg ***********************/
/*            Put a message into a shared memory region.	     */
/*            Assigns a transport-layer sequence number.             */
/*********************************************************************/

int tport_putmsg( SHM_INFO *region,    /* info structure for memory region    */
		  MSG_LOGO *putlogo,   /* type,module,instid of incoming msg  */
		  long      length,    /* size of incoming message            */
		  char     *msg )      /* pointer to incoming message         */
{
   volatile static MSG_TRACK  trak[NTRACK_PUT];   /* sequence number keeper   */
   volatile static int        nlogo;              /* # of logos seen so far   */
   int               	      it;                 /* index into trak          */
   struct sembuf     sops;             /* semaphore operations; 
                                             changed to non-static 980424:ldd */
   SHM_HEAD         *shm;              /* pointer to start of memory region   */
   char             *ring;             /* pointer to ring part of memory      */
   long              ir;               /* index into memory ring              */
   long              nfill;            /* # bytes from ir to ring's last-byte */
   long              nwrap;            /* # bytes to wrap to front of ring    */
   TPORT_HEAD        hd;               /* transport layer header to put       */
   char             *h;                /* pointer to transport layer header   */
   TPORT_HEAD        old;              /* transport header of oldest msg      */
   char             *o;                /* pointer to oldest transport header  */
   int j;
   int res;
   int retval = PUT_OK;                /* return value for this function      */

/**** First time around, init the sequence counters, semaphore controls ****/

   if (Put_Init)
   {
       nlogo    = 0;
       
       for( j=0 ; j < NTRACK_PUT ; j++ )
       {
          trak[j].memkey      = 0;
          trak[j].logo.type   = 0;
          trak[j].logo.mod    = 0;
          trak[j].logo.instid = 0;
          trak[j].seq         = 0;
          trak[j].keyout      = 0;
       }

       Put_Init = 0;
   }

   sops.sem_num = 0;   /* moved outside Put_Init loop 980424:ldd */
   sops.sem_op  = 0;   /* moved outside Put_Init loop 980424:ldd */
   sops.sem_flg = 0;   /* moved outside Put_Init loop 980424:ldd */ 

/**** Set up pointers for shared memory, etc. ****/

   shm  = region->addr;
   ring = (char *) shm + sizeof(SHM_HEAD);
   h    = (char *) (&hd);
   o    = (char *) (&old);

/**** First, see if the incoming message will fit in the memory region ****/

   if ( length + sizeof(TPORT_HEAD) > shm->keymax )
   {
      fprintf( stdout, 
              "ERROR: tport_putmsg; message too large (%ld) for Region %ld\n",
               length, region->key);
      return( PUT_TOOBIG ); 
   }
   
/**** Block signals that Earthworm uses to kill processes and threads ****/

   tport_blocksigs();

/**** Change semaphore; let others know you're using tracking structure & memory  ****/

   sops.sem_op = SHM_INUSE;
   res = semop( region->sid, &sops, 1 );   
   if (res == -1)
      tport_syserr( "tport_putmsg semop ->inuse", region->key );

/**** Next, find incoming logo in list of combinations already seen ****/

   for( it=0 ; it < nlogo ; it++ )
   {   
      if ( region->key     != trak[it].memkey      )  continue;
      if ( putlogo->type   != trak[it].logo.type   )  continue;
      if ( putlogo->mod    != trak[it].logo.mod    )  continue;
      if ( putlogo->instid != trak[it].logo.instid )  continue;
      goto build_header;
   }

/**** Incoming logo is a new combination; store it, if there's room ****/

   if ( nlogo == NTRACK_PUT )
   {
      fprintf( stdout, 
              "ERROR: tport_putmsg; exceeded NTRACK_PUT, msg not sent\n");
      retval = PUT_NOTRACK;
      goto release_semaphore; 
   }
   it = nlogo;
   trak[it].memkey =  region->key;
   trak[it].logo   = *putlogo;
   nlogo++;

/**** Store everything you need in the transport header ****/

build_header:
   hd.start = FIRST_BYTE;
   hd.size  = length;
   hd.logo  = trak[it].logo;
   hd.seq   = trak[it].seq++;

/**** In shared memory, see if keyin will wrap; if so, reset keyin and keyold ****/

   if ( shm->keyin + sizeof(TPORT_HEAD) + length  <  shm->keyold )
   {
       shm->keyin  = shm->keyin  % shm->keymax;
       shm->keyold = shm->keyold % shm->keymax;
       if ( shm->keyin <= shm->keyold ) shm->keyin += shm->keymax;
     /*fprintf( stdout, 
               "NOTICE: tport_putmsg; keyin wrapped & reset; Region %ld\n",
                region->key );*/
   }

/**** Then see if there's enough room for new message in shared memory ****/
/****      If not, "delete" oldest messages until there's room         ****/

   while( shm->keyin + sizeof(TPORT_HEAD) + length - shm->keyold > shm->keymax )
   {
      ir = shm->keyold % shm->keymax;
      if ( ring[ir] != FIRST_BYTE )
      {
          fprintf( stdout, 
                  "ERROR: tport_putmsg; keyold not at FIRST_BYTE, Region %ld\n",
                   region->key );
          retval = TPORT_FATAL;
          goto release_semaphore; 
      }
      for ( j=0 ; j < sizeof(TPORT_HEAD) ; j++ )
      {
         if ( ir >= shm->keymax )   ir -= shm->keymax;
         o[j] = ring[ir++];
      } 
      shm->keyold += sizeof(TPORT_HEAD) + old.size;
   }

/**** Now copy transport header into shared memory by chunks... ****/

   ir = shm->keyin % shm->keymax;
   nwrap = ir + sizeof(TPORT_HEAD) - shm->keymax;
   if ( nwrap <= 0 )
   {
         memcpy( (void *) &ring[ir], (void *) h, sizeof(TPORT_HEAD) );
   }
   else
   {
         nfill = sizeof(TPORT_HEAD) - nwrap;
         memcpy( (void *) &ring[ir], (void *) &h[0],     nfill );
         memcpy( (void *) &ring[0],  (void *) &h[nfill], nwrap );
   }
   ir += sizeof(TPORT_HEAD);
   if ( ir >= shm->keymax )  ir -= shm->keymax;

/**** ...and copy message into shared memory by chunks ****/

   nwrap = ir + length - shm->keymax;
   if ( nwrap <= 0 )
   {
         memcpy( (void *) &ring[ir], (void *) msg, length );
   }
   else
   {
         nfill = length - nwrap;
         memcpy( (void *) &ring[ir], (void *) &msg[0],     nfill );
         memcpy( (void *) &ring[0],  (void *) &msg[nfill], nwrap );
   }
   shm->keyin += sizeof(TPORT_HEAD) + length;

/**** Finished with shared memory, let others know via semaphore ****/

release_semaphore:
   sops.sem_op = SHM_FREE;
   res = semop( region->sid, &sops, 1 );  
   if (res == -1)
      tport_syserr( "tport_putmsg semop ->free", region->key ); 

/**** Release signals that Earthworm uses to kill processes and threads ****/

   tport_releasesigs();

   if( retval == TPORT_FATAL ) exit( 1 );
   return( retval ); 
}


/******************** function tport_getmsg_base**********************/
/*     Find (and possibly get) a message out of shared memory.       */
/*********************************************************************/

static int tport_getmsg_base( 
          SHM_INFO  *region,   /* info structure for memory region  */
		  MSG_LOGO  *getlogo,  /* requested logo(s)                 */
		  short      nget,     /* number of logos in getlogo        */
		  MSG_LOGO  *logo,     /* logo of retrieved msg 	    */
		  long      *length,   /* size of retrieved message         */
		  char      *msg,      /* retrieved message                 */
		  long       maxsize,  /* max length for retrieved message  */
		  char       dont_copy)/* if <>0, don't copy message into msg */
{
   static MSG_TRACK  trak[NTRACK_GET]; /* sequence #, outpointer keeper     */
   static int        nlogo;            /* # modid,type,instid combos so far */
   int               it;               /* index into trak                   */
   SHM_HEAD         *shm;              /* pointer to start of memory region */
   char             *ring;             /* pointer to ring part of memory    */
   TPORT_HEAD       *tmphd;            /* temp pointer into shared memory   */
   long              ir;               /* index into the ring               */
   long              nfill;            /* bytes from ir to ring's last-byte */
   long              nwrap;            /* bytes to grab from front of ring  */
   TPORT_HEAD        hd;               /* transport header from memory      */
   char             *h;                /* pointer to transport layer header */
   int               ih;               /* index into the transport header   */
   unsigned long     keyin;            /* in-pointer to shared memory       */
   unsigned long     keyold;           /* oldest complete message in memory */
   unsigned long     keyget;           /* pointer at which to start search  */
   int               status = GET_OK;  /* how did retrieval go?             */
   int               trakked;          /* flag for trakking list entries    */
   int               i,j;

/**** Get the pointers set up ****/

   shm  = region->addr;
   ring = (char *) shm + sizeof(SHM_HEAD);
   h    = (char *) (&hd);

/**** First time around, initialize sequence counters, outpointers ****/

   if (Get_Init)
   {
       nlogo = 0;  

       for( i=0 ; i < NTRACK_GET ; i++ )
       {
          trak[i].memkey      = 0;
          trak[i].logo.type   = 0;
          trak[i].logo.mod    = 0;
          trak[i].logo.instid = 0;
          trak[i].seq         = 0;
          trak[i].keyout      = 0;
          trak[i].active      = 0; /*960618:ldd*/
       }
       Get_Init = 0;
   }

/**** make sure all requested logos are entered in tracking list ****/

   for ( j=0 ; j < nget ; j++ )  /* for all requested message logos */
   {
       trakked = 0;  /* assume it's not being trakked */
       for( it=0 ; it < nlogo ; it++ )  /* for all logos we're tracking */
       {
          if( region->key       != trak[it].memkey      ) continue;
          if( getlogo[j].type   != trak[it].logo.type   ) continue;
          if( getlogo[j].mod    != trak[it].logo.mod    ) continue;
          if( getlogo[j].instid != trak[it].logo.instid ) continue;
          trakked = 1;  /* found it in the trakking list! */
          break;
       }
       if( trakked ) continue;  
    /* Make an entry in trak for this logo; if there's room */
       if ( nlogo < NTRACK_GET ) 
       {
          it = nlogo;
          trak[it].memkey = region->key;
          trak[it].logo   = getlogo[j];  
          nlogo++;  
       }       
   }

/**** find latest starting index to look for any of the requested logos ****/

findkey:

   keyget = shm->keyold;

   for ( it=0 ; it < nlogo ; it++ )  /* for all message logos we're tracking */
   {
       if ( trak[it].memkey != region->key ) continue;
       for ( j=0 ; j < nget ; j++ )  /* for all requested message logos */
       {
          if((getlogo[j].type   == trak[it].logo.type   || getlogo[j].type==WILD) &&
             (getlogo[j].mod    == trak[it].logo.mod    || getlogo[j].mod==WILD)  &&
             (getlogo[j].instid == trak[it].logo.instid || getlogo[j].instid==WILD) )
          {
             if ( trak[it].keyout > keyget )  keyget = trak[it].keyout;
          }
       }
    }
    keyin = shm->keyin;

/**** See if keyin and keyold were wrapped and reset by tport_putmsg; ****/
/****       If so, reset trak[xx].keyout and go back to findkey       ****/

   if ( keyget > keyin )
   {
      keyold = shm->keyold; 
      for ( it=0 ; it < nlogo ; it++ )  
      {
         if( trak[it].memkey == region->key ) 
         {
          /* reset keyout */
/*DEBUG*/    /*printf("tport_getmsg: Pre-reset:  keyout=%10u    keyold=%10u  keyin=%10u\n",
                     trak[it].keyout, keyold, keyin );*/
             trak[it].keyout = trak[it].keyout % shm->keymax;
/*DEBUG*/    /*printf("tport_getmsg:  Intermed:  keyout=%10u    keyold=%10u  keyin=%10u\n",
                     trak[it].keyout, keyold, keyin );*/

          /* make sure new keyout points to keyin or to a msg's first-byte; */
          /* if not, we've been lapped, so set keyout to keyold             */
             ir    = trak[it].keyout; 
             tmphd = (TPORT_HEAD *) &ring[ir];  
             if ( trak[it].keyout == keyin   ||   
                  (keyin-trak[it].keyout)%shm->keymax == 0 ) 
             {
/*DEBUG*/       /*printf("tport_getmsg:  Intermed:  keyout=%10u  same as keyin\n",
                       trak[it].keyout );*/
                trak[it].keyout = keyin;
             }
             else if( tmphd->start != FIRST_BYTE )
             {  
/*DEBUG*/       /*printf("tport_getmsg:  Intermed:  keyout=%10u  does not point to FIRST_BYTE\n",
                        trak[it].keyout );*/
                trak[it].keyout = keyold;
             }

          /* else, make sure keyout's value is between keyold and keyin */
             else if ( trak[it].keyout < keyold ) 
             {
                do {
                    trak[it].keyout += shm->keymax;
                } while ( trak[it].keyout < keyold );
             }
/*DEBUG*/    /*printf("tport_getmsg:     Reset:  keyout=%10u    keyold=%10u  keyin=%10u\n",
                     trak[it].keyout, keyold, keyin );*/
         }
      }
    /*fprintf( stdout, 
          "NOTICE: tport_getmsg; keyin wrapped, keyout(s) reset; Region %ld\n",
           region->key );*/

      goto findkey;
   }
     

/**** Find next message from requested type, module, instid ****/

nextmsg:

   while ( keyget < keyin )
   {
   /* make sure you haven't been lapped by tport_putmsg */
       if ( keyget < shm->keyold ) keyget = shm->keyold;

   /* load next header; make sure you weren't lapped */
       ir = keyget % shm->keymax;
       for ( ih=0 ; ih < sizeof(TPORT_HEAD) ; ih++ )    
       {
          if ( ir >= shm->keymax )  ir -= shm->keymax;
          h[ih] = ring[ir++];
       }
       if ( keyget < shm->keyold ) continue;  /*added 960612:ldd*/

   /* make sure it starts at beginning of a header */
       if ( hd.start != FIRST_BYTE ) 
       {
          fprintf( stdout, 
                  "ERROR: tport_getmsg; keyget not at FIRST_BYTE, Region %ld\n",
                   region->key );
          exit( 1 );
       }
       keyget += sizeof(TPORT_HEAD) + hd.size;

   /* see if this msg matches any requested type */
       for ( j=0 ; j < nget ; j++ )  
       {
          if((getlogo[j].type   == hd.logo.type   || getlogo[j].type == WILD) &&
             (getlogo[j].mod    == hd.logo.mod    || getlogo[j].mod  == WILD) &&
             (getlogo[j].instid == hd.logo.instid || getlogo[j].instid == WILD) )
          {
   
/**** Found a message of requested logo; retrieve it! ****/ 
        /* complain if retreived msg is too big */      
             if ( !dont_copy && (hd.size > maxsize) )  
             {
               *logo   = hd.logo;
               *length = hd.size;
                status = GET_TOOBIG; 
                goto trackit;    /*changed 960612:ldd*/
             }
        /* copy message by chunks to caller's address */
             nwrap = ir + hd.size - shm->keymax;
             if ( nwrap <= 0 )
             {
                if ( !dont_copy )
                	memcpy( (void *) msg, (void *) &ring[ir], hd.size );
             }
             else
             {
                nfill = hd.size - nwrap;
                if ( !dont_copy ) {
	                memcpy( (void *) &msg[0],     (void *) &ring[ir], nfill );
    	            memcpy( (void *) &msg[nfill], (void *) &ring[0],  nwrap );
    	        }
             }
        /* see if we got run over by tport_putmsg while copying msg */
        /* if we did, go back and try to get a msg cleanly          */
             keyold = shm->keyold; 
             if ( keyold >= keyget ) 
             {
                keyget = keyold;
                goto nextmsg;
             }

        /* set other returned variables */
            *logo   = hd.logo;
            if ( length != NULL )
	            *length = hd.size;

trackit:
        /* find msg logo in tracked list */
             for ( it=0 ; it < nlogo ; it++ )  
             {
                if ( region->key    != trak[it].memkey      )  continue;
                if ( hd.logo.type   != trak[it].logo.type   )  continue;
                if ( hd.logo.mod    != trak[it].logo.mod    )  continue;
                if ( hd.logo.instid != trak[it].logo.instid )  continue;
                /* activate sequence tracking if 1st msg */  
                if ( !trak[it].active )
                {
                    trak[it].seq    = hd.seq;
                    trak[it].active = 1;
                }
                goto sequence;
             }
        /* new logo, track it if there's room */   
             if ( nlogo == NTRACK_GET ) 
             {
                fprintf( stdout, 
                     "ERROR: tport_getmsg; exceeded NTRACK_GET\n"); 
                if( status != GET_TOOBIG ) status = GET_NOTRACK; /*changed 960612:ldd*/
                goto wrapup;
             }
             it = nlogo;
             trak[it].memkey = region->key;
             trak[it].logo   = hd.logo;  
             trak[it].seq    = hd.seq;
             trak[it].active = 1;      /*960618:ldd*/
             nlogo++;         

sequence:
        /* check if sequence #'s match; update sequence # */
             if ( status == GET_TOOBIG   )  goto wrapup; /*added 960612:ldd*/     
             if ( hd.seq != trak[it].seq ) 
             {
                status = GET_MISS; 
                trak[it].seq = hd.seq;
             }
             trak[it].seq++;
 
        /* Ok, we're finished grabbing this one */
             goto wrapup;

          } /* end if of logo & getlogo match */
       }    /* end for over getlogo */
   }        /* end while over ring */

/**** If you got here, there were no messages of requested logo(s) ****/

   status = GET_NONE;

/**** update outpointer (->msg after retrieved one) for all requested logos ****/

wrapup:
   for ( it=0 ; it < nlogo ; it++ )  /* for all message logos we're tracking */
   {
       if ( trak[it].memkey != region->key ) continue;
       for ( j=0 ; j < nget ; j++ )  /* for all requested message logos */
       {
          if((getlogo[j].type   == trak[it].logo.type   || getlogo[j].type==WILD) &&
             (getlogo[j].mod    == trak[it].logo.mod    || getlogo[j].mod==WILD)  &&
             (getlogo[j].instid == trak[it].logo.instid || getlogo[j].instid==WILD) )
          {
             trak[it].keyout = keyget;
          }
       }
    }

   return( status );     

}


/*********************** function tport_flush ************************/
/*                 Flush messages out of shared memory.              */
/*********************************************************************/

int tport_flush( SHM_INFO  *region,   /* info structure for memory region  */
		  MSG_LOGO  *getlogo,  /* requested logo(s)                 */
		  short      nget,     /* number of logos in getlogo        */
		  MSG_LOGO  *logo )     /* logo of retrieved msg 	    */
{
	int res;
	
	do{
		res = tport_getmsg_base( region, getlogo, nget, logo, NULL, NULL, 0, 1 );
	} while (res !=GET_NONE);
	return res;
}


/*********************** function tport_getmsg ***********************/
/*                 Get a message out of shared memory.               */
/*********************************************************************/

int tport_getmsg( SHM_INFO  *region,   /* info structure for memory region  */
		  MSG_LOGO  *getlogo,  /* requested logo(s)                 */
		  short      nget,     /* number of logos in getlogo        */
		  MSG_LOGO  *logo,     /* logo of retrieved msg 	    */
		  long      *length,   /* size of retrieved message         */
		  char      *msg,      /* retrieved message                 */
		  long       maxsize ) /* max length for retrieved message  */
{
	return tport_getmsg_base( region, getlogo, nget, 
		logo, length, msg, maxsize, 0 );
}


/*********************** function tport_doFlagOp *********************/
/*                  Perform operation op on the flag                 */
/*********************************************************************/

static int tport_doFlagOp( SHM_INFO* region, int pid, int op )  
{
   int i;
   SHM_FLAG  *smf;
   int start, stop;
   int		 ret_val = 0;
   int res;
   struct sembuf     sops;             /* semaphore operations; */

   if ( Flag_Init )
   	  tport_createFlag();
   smf = (SHM_FLAG*)smf_region.addr;
   if ( smf == NULL )
      return 0;

/**** Block signals that Earthworm uses to kill processes and threads ****/

   tport_blocksigs();

/**** Change semaphore; let others know you're using tracking structure & memory  ****/

   sops.sem_num = 0;
   sops.sem_flg = 0;
   sops.sem_op = SHM_INUSE;
   res = semop( smf_region.sid, &sops, 1 );   
   if (res == -1)
      tport_syserr( "tport_doFlagOp semop ->inuse", region->key );

   switch ( op ) {
   case FF_REMOVE:
   	start = 0;
   	stop = smf->nPids;
   	break;
   	
   case FF_FLAG2ADD:
   	start = 0;
   	stop = smf->nPidsToDie;
   	break;
   	
   case FF_FLAG2DIE:
    if ( pid == TERMINATE ) {
	   	/* Terminate supercedes all individial process termination requests */
	   	if ( smf->nPidsToDie == 0 || smf->pid[0] != TERMINATE ) {
			smf->pid[smf->nPids] = smf->pid[0];
			smf->pid[0] = TERMINATE;
			smf->nPidsToDie++;
			smf->nPids++;
		}
		if ( region != NULL )
			(region->addr)->flag = TERMINATE;
	   	ret_val = TERMINATE;
	   	start = 0;
	   	stop = 0;
	} else if ( smf->nPidsToDie > 0 && smf->pid[0] == TERMINATE ) {
	   	/* We've already been told to terminate everybody */
	   	ret_val = pid;
	   	start = 0;
	   	stop = 0;
	} else {
	   	start = smf->nPidsToDie;
   		stop = smf->nPids;
   	}
   	break;
   	
   case FF_GETFLAG:
  	if ( smf->nPidsToDie>0 && smf->pid[0] == TERMINATE ) {
   		/* All modules being terminated */
   		ret_val = TERMINATE;
   	}
   	start = 0;
   	stop = smf->nPidsToDie;
   	break;
   	
   case FF_CLASSIFY:
   	start = 0;
   	stop = smf->nPids;
   	break;
   }

   for ( i=start; i<stop; i++ )
		if ( smf->pid[i] == pid )
			break;
	
   switch ( op ) {
   case FF_REMOVE:
	if ( i >= stop ) {
		ret_val = 0;
		break;
	}
	if ( i < smf->nPidsToDie ) {
		smf->nPidsToDie--;
		smf->pid[i] = smf->pid[smf->nPidsToDie];
		i = smf->nPidsToDie;
	}
	smf->nPids--;
	smf->pid[smf->nPidsToDie] = smf->pid[smf->nPids];
	ret_val = pid;
	break;
	
   case FF_FLAG2ADD:
	if ( i >= stop ) {
		/* If no room, we'll have to treat as old-style */
		if ( smf->nPids < MAX_NEWTPROC ) {
			smf->pid[smf->nPids] = pid;
			smf->nPids++;
		}
	}
	ret_val = pid;
	break;
	
   case FF_FLAG2DIE:
   	if ( ret_val == 0 ) {
		if ( i >= stop ) {
			if ( region != NULL )
				(region->addr)->flag = pid;
		} else if ( i > smf->nPidsToDie ) {
			smf->pid[i] = smf->pid[smf->nPidsToDie];
			smf->pid[smf->nPidsToDie] = pid;
			smf->nPidsToDie++;
		}
		ret_val = pid;
	}
	break;

   case FF_GETFLAG:
    if ( ret_val != TERMINATE )
		if ( i >= stop ) {
			stop = smf->nPids;
			for ( ; i<stop; i++ )
				if ( smf->pid[i] == pid )
					break;
			/* If pid unknown, treat as an old-style module */
			ret_val = ( i >= stop && region != NULL) ? (region->addr)->flag : 0;
		} else
			ret_val = ( i >= stop ) ? 0 : pid;
	break;
	
   case FF_CLASSIFY:
	ret_val = ( i >= stop ) ? 0 : ( i >= smf->nPidsToDie ) ? 1 : 2;
   }

/**** Finished with shared memory, let others know via semaphore ****/

   sops.sem_op = SHM_FREE;
   res = semop( smf_region.sid, &sops, 1 );  
   if (res == -1)
      tport_syserr( "tport_doFlagOp semop ->free", region->key ); 

/**** Release signals that Earthworm uses to kill processes and threads ****/

   tport_releasesigs();

   return ret_val;
}


/********************* function tport_putflag ************************/
/*           Puts the kill flag into a shared memory region.         */
/*********************************************************************/

void tport_putflag( SHM_INFO *region,  /* shared memory info structure */
       		    int     flag )   /* tells attached processes to exit */
{
	if ( smf_region.addr == NULL )
		(region->addr)->flag = flag;
	else
		tport_doFlagOp( region, flag, FF_FLAG2DIE );
}


/******************** function tport_detachFromFlag ******************/
/* Remove pid from flag; return 0 if pid not in flag, pid otherwise  */
/*********************************************************************/
int tport_detachFromFlag( SHM_INFO *region, int pid ) 	
{
	return tport_doFlagOp( region, pid, FF_REMOVE );
}


/*********************** function tport_addToFlag ********************/
/*   Add pid from flag; return 0 if pid not in flag, pid otherwise   */
/*********************************************************************/

int tport_addToFlag( SHM_INFO *region, int pid ) 	
{
	return tport_doFlagOp( region, pid, FF_FLAG2ADD );
}



/*********************** function tport_getflag **********************/
/*         Returns the kill flag from a shared memory region.        */
/*********************************************************************/

int tport_getflag( SHM_INFO *region )  
      
{
	if ( smf_region.addr == NULL )
		return (region->addr)->flag;
	else
		return tport_doFlagOp( region, getpid(), FF_GETFLAG );
}


/*********************** function tport_newModule ********************/
/* Returns 0 if process w/ id pid isn't using new transport library  */
/*********************************************************************/

int tport_newModule( int pid )  
      
{
	return (tport_doFlagOp( NULL, pid, FF_CLASSIFY ) != 0);
}



/************************** tport_bufthr ****************************/
/*     Thread to buffer input from one transport ring to another.   */
/********************************************************************/
void *tport_bufthr( void *dummy )
{
   char		 errnote[150];
   MSG_LOGO      logo;
   long          msgsize;
   unsigned char msgseq;
   int           res1, res2;
   int 		 gotmsg;

/* Flush all existing messages from the public memory region
   *********************************************************/
   while( tport_copyfrom((SHM_INFO *) PubRegion, (MSG_LOGO *) Getlogo, 
                          Nget, &logo, &msgsize, (char *) Message, 
                          MaxMsgSize, &msgseq )  !=  GET_NONE  );

   while ( 1 )
   {
/* If a terminate flag is found, go to sleep; 
   the main thread should cause the process to exit!
   *************************************************/
      if ( tport_getflag( (SHM_INFO *) PubRegion ) == TERMINATE ||
           tport_getflag( (SHM_INFO *) BufRegion ) == TERMINATE    )
      {
         tport_putflag( (SHM_INFO *) BufRegion, TERMINATE );
         sleep_ew( 100000 );
      /* thr_exit( NULL ); */ /*let the main thread exit!*/
      }

      do 
      {
/* Try to copy a message from the public memory region
   ***************************************************/
 	  res1 = tport_copyfrom((SHM_INFO *) PubRegion, (MSG_LOGO *) Getlogo, 
				Nget, &logo, &msgsize, (char *) Message, 
			        MaxMsgSize, &msgseq );
	  gotmsg = 1;

/* Handle return values
   ********************/
          switch ( res1 ) 
	  {
          case GET_MISS_LAPPED:
		sprintf( errnote, 
			"tport_bufthr: Missed msg(s)  c%d m%d t%d  Overwritten, region:%ld.",
			 (int) logo.instid, (int) logo.mod, (int) logo.type,
			 PubRegion->key );
		tport_buferror( ERR_LAPPED, errnote );
		break;
	  case GET_MISS_SEQGAP:
		sprintf( errnote, 
			"tport_bufthr: Missed msg(s)  c%d m%d t%d  Sequence gap, region:%ld.",
			 (int) logo.instid, (int) logo.mod, (int) logo.type,
			 PubRegion->key );
		tport_buferror( ERR_SEQGAP, errnote );
		break;
          case GET_NOTRACK:
		sprintf( errnote,
			"tport_bufthr: Logo c%d m%d t%d not tracked; NTRACK_GET exceeded.", 
			(int) logo.instid, (int) logo.mod, (int) logo.type );
		tport_buferror( ERR_UNTRACKED, errnote );
          case GET_OK:
		break;
          case GET_TOOBIG:
		sprintf( errnote, 
			"tport_bufthr: msg[%ld] c%d m%d t%d seq%d too big; skipped in region:%ld.",
			 msgsize, (int) logo.instid, (int) logo.mod, 
		         (int) logo.type, (int) msgseq, PubRegion->key );
		tport_buferror( ERR_OVERFLOW, errnote );
          case GET_NONE:
		gotmsg = 0;
		break;
          }

/* If you did get a message, copy it to private ring
   *************************************************/  
	  if ( gotmsg )  
	  {        
	        res2 = tport_copyto( (SHM_INFO *) BufRegion, &logo, 
			             msgsize, (char *) Message, msgseq );
		switch (res2) 
		{
		case PUT_TOOBIG:
		   sprintf( errnote, 
		       "tport_bufthr: msg[%ld] (c%d m%d t%d) too big for Region:%ld.",
			msgsize, (int) logo.instid, (int) logo.mod, (int) logo.type,
			BufRegion->key );
		   tport_buferror( ERR_OVERFLOW, errnote );
		case PUT_OK:
		   break;
		}
	  }
      } while ( res1 != GET_NONE ); 

      sleep_ew( 500 );

   }
   return NULL;
}
 
 
/************************** tport_buffer ****************************/
/*       Function to initialize the input buffering thread          */
/********************************************************************/
int tport_buffer( SHM_INFO  *region1,      /* transport ring	         */
		  SHM_INFO  *region2,      /* private ring 	         */
		  MSG_LOGO  *getlogo,      /* array of logos to copy 	 */
		  short      nget,         /* number of logos in getlogo */
		  unsigned   maxMsgSize,   /* size of message buffer 	 */
		  unsigned char module,	   /* module id of main thread   */
		  unsigned char instid )   /* instid id of main thread   */
{
   char errmsg[80];

/* Allocate message buffer
   ***********************/
   Message = (char *) malloc( maxMsgSize );
   if ( Message == NULL )
   {
      fprintf( stdout, "tport_buffer: Error allocating message buffer\n" );
      return( -1 );
   }

/* Copy function arguments to global variables
   *******************************************/
   PubRegion   = region1;
   BufRegion   = region2;
   Getlogo     = getlogo;
   Nget        = nget;
   MaxMsgSize  = maxMsgSize;
   MyModuleId  = module;
   MyInstid    = instid;

/* Lookup message type for error messages
   **************************************/
   if ( GetType( "TYPE_ERROR", &TypeError ) != 0 ) {
      fprintf( stderr, 
              "tport_buffer: Invalid message type <TYPE_ERROR>\n" );
      return( -1 );
   }

/* Start the input buffer thread
   *****************************/
   if ( thr_create( NULL, NULL, tport_bufthr, NULL, THR_DETACHED,
                    NULL ) != 0 )
   {
      sprintf( errmsg, "thr_create errno: %d", errno );
      perror( errmsg );
      return( -1 );
   }

/* Yield to the buffer thread
   **************************/
   thr_yield();
   return( 0 );
}


/********************** function tport_copyfrom *********************/
/*      get a message out of public shared memory; save the 	    */
/*     sequence number from the transport layer, with the intent    */
/*       of copying it to a private (buffering) memory ring         */
/********************************************************************/

int tport_copyfrom( SHM_INFO  *region,   /* info structure for memory region */
		    MSG_LOGO  *getlogo,  /* requested logo(s)                */
		    short      nget,     /* number of logos in getlogo       */
		    MSG_LOGO  *logo,     /* logo of retrieved message 	     */
		    long      *length,   /* size of retrieved message        */
		    char      *msg,      /* retrieved message                */
		    long       maxsize,  /* max length for retrieved message */
		    unsigned char *seq ) /* TPORT_HEAD seq# of retrieved msg */
{
   static MSG_TRACK  trak[NTRACK_GET]; /* sequence #, outpointer keeper     */
   static int        nlogo;            /* # modid,type,instid combos so far */
   int               it;               /* index into trak                   */
   SHM_HEAD         *shm;              /* pointer to start of memory region */
   char             *ring;             /* pointer to ring part of memory    */
   TPORT_HEAD       *tmphd;            /* temp pointer into shared memory   */
   long              ir;               /* index into the ring               */
   long              nfill;            /* bytes from ir to ring's last-byte */
   long              nwrap;            /* bytes to grab from front of ring  */
   TPORT_HEAD        hd;               /* transport header from memory      */
   char             *h;                /* pointer to transport layer header */
   int               ih;               /* index into the transport header   */
   unsigned long     keyin;            /* in-pointer to shared memory       */
   unsigned long     keyold;           /* oldest complete message in memory */
   unsigned long     keyget;           /* pointer at which to start search  */
   int               status = GET_OK;  /* how did retrieval go?             */
   int               lapped = 0;       /* = 1 if memory ring has been over- */
 				       /* written since last tport_copyfrom */
   int               trakked;          /* flag for trakking list entries    */
   int               i,j;

/**** Get the pointers set up ****/

   shm  = region->addr;
   ring = (char *) shm + sizeof(SHM_HEAD);
   h    = (char *) (&hd);

/**** First time around, initialize sequence counters, outpointers ****/

   if (Copyfrom_Init)
   {
       nlogo = 0;  

       for( i=0 ; i < NTRACK_GET ; i++ )
       {
          trak[i].memkey      = 0;
          trak[i].logo.type   = 0;
          trak[i].logo.mod    = 0;
          trak[i].logo.instid = 0;
          trak[i].seq         = 0;
          trak[i].keyout      = 0; 
          trak[i].active      = 0; /*960618:ldd*/
       }
       Copyfrom_Init = 0;
   }

/**** make sure all requested logos are entered in tracking list ****/

   for ( j=0 ; j < nget ; j++ )  /* for all requested message logos */
   {
       trakked = 0;  /* assume it's not being trakked */
       for( it=0 ; it < nlogo ; it++ )  /* for all logos we're tracking */
       {
          if( region->key       != trak[it].memkey      ) continue;
          if( getlogo[j].type   != trak[it].logo.type   ) continue;
          if( getlogo[j].mod    != trak[it].logo.mod    ) continue;
          if( getlogo[j].instid != trak[it].logo.instid ) continue;
          trakked = 1;  /* found it in the trakking list! */
          break;
       }
       if( trakked ) continue;  
    /* Make an entry in trak for this logo; if there's room */
       if ( nlogo < NTRACK_GET ) 
       {
          it = nlogo;
          trak[it].memkey = region->key;
          trak[it].logo   = getlogo[j];  
          nlogo++;  
       }       
   }

/**** find latest starting index to look for any of the requested logos ****/

findkey:

   keyget = 0;

   for ( it=0 ; it < nlogo ; it++ )  /* for all message logos we're tracking */
   {
       if ( trak[it].memkey != region->key ) continue;
       for ( j=0 ; j < nget ; j++ )  /* for all requested message logos */
       {
          if((getlogo[j].type   == trak[it].logo.type   || getlogo[j].type==WILD) &&
             (getlogo[j].mod    == trak[it].logo.mod    || getlogo[j].mod==WILD)  &&
             (getlogo[j].instid == trak[it].logo.instid || getlogo[j].instid==WILD) )
          {
             if ( trak[it].keyout > keyget )  keyget = trak[it].keyout;
          }
       }
   }

/**** make sure you haven't been lapped by tport_copyto or tport_putmsg ****/
   if ( keyget < shm->keyold ) {
      keyget = shm->keyold;
      lapped = 1;   
   }

/**** See if keyin and keyold were wrapped and reset by tport_putmsg; ****/
/****       If so, reset trak[xx].keyout and go back to findkey       ****/

   keyin = shm->keyin;
   if ( keyget > keyin )
   {
      keyold = shm->keyold;  
      for ( it=0 ; it < nlogo ; it++ )  
      {
         if( trak[it].memkey == region->key ) 
         {
          /* reset keyout */
/*DEBUG*/    /*printf("tport_copyfrom: Pre-reset:  keyout=%10u    keyold=%10u  keyin=%10u\n",
                     trak[it].keyout, keyold, keyin );*/
             trak[it].keyout = trak[it].keyout % shm->keymax;
/*DEBUG*/    /*printf("tport_copyfrom:  Intermed:  keyout=%10u    keyold=%10u  keyin=%10u\n",
                     trak[it].keyout, keyold, keyin );*/

          /* make sure new keyout points to keyin or to a msg's first-byte; */
          /* if not, we've been lapped, so set keyout to keyold             */
             ir    = trak[it].keyout; 
             tmphd = (TPORT_HEAD *) &ring[ir];  
             if ( trak[it].keyout == keyin   ||   
                  (keyin-trak[it].keyout)%shm->keymax == 0 ) 
             {
/*DEBUG*/       /*printf("tport_copyfrom:  Intermed:  keyout=%10u  same as keyin\n",
                        trak[it].keyout );*/
                trak[it].keyout = keyin;
             }
             else if( tmphd->start != FIRST_BYTE )
             {  
/*DEBUG*/       /*printf("tport_copyfrom:  Intermed:  keyout=%10u  does not point to FIRST_BYTE\n",
                        trak[it].keyout );*/
                trak[it].keyout = keyold;
                lapped = 1;   
             }

          /* else, make sure keyout's value is between keyold and keyin */
             else if ( trak[it].keyout < keyold ) 
             { 
                do {
                    trak[it].keyout += shm->keymax;
                } while ( trak[it].keyout < keyold );
             }
/*DEBUG*/    /*printf("tport_copyfrom:     Reset:  keyout=%10u    keyold=%10u  keyin=%10u\n",
                     trak[it].keyout, keyold, keyin );*/
         }
      }
    /*fprintf( stdout, 
          "NOTICE: tport_copyfrom; keyin wrapped, keyout(s) reset; Region %ld\n",
           region->key );*/

      goto findkey;
   }
     

/**** Find next message from requested type, module, instid ****/

nextmsg:

   while ( keyget < keyin )
   {
   /* make sure you haven't been lapped by tport_copyto or tport_putmsg */
       if ( keyget < shm->keyold ) {
	  keyget = shm->keyold;
          lapped = 1;
       }

   /* load next header; make sure you weren't lapped */
       ir = keyget % shm->keymax;
       for ( ih=0 ; ih < sizeof(TPORT_HEAD) ; ih++ )    
       {
          if ( ir >= shm->keymax )  ir -= shm->keymax;
          h[ih] = ring[ir++];
       }
       if ( keyget < shm->keyold ) continue;  /*added 960612:ldd*/

   /* make sure it starts at beginning of a header */
       if ( hd.start != FIRST_BYTE ) 
       {
          fprintf( stdout, 
                  "ERROR: tport_copyfrom; keyget not at FIRST_BYTE, Region %ld\n",
                   region->key );
          exit( 1 );
       }
       keyget += sizeof(TPORT_HEAD) + hd.size;

   /* see if this msg matches any requested type */
       for ( j=0 ; j < nget ; j++ )  
       {
          if((getlogo[j].type   == hd.logo.type   || getlogo[j].type == WILD) &&
             (getlogo[j].mod    == hd.logo.mod    || getlogo[j].mod  == WILD) &&
             (getlogo[j].instid == hd.logo.instid || getlogo[j].instid == WILD) )
          {
   
/**** Found a message of requested logo; retrieve it! ****/ 
        /* complain if retreived msg is too big */      
             if ( hd.size > maxsize )  
             {
               *logo   = hd.logo;
               *length = hd.size;
	       *seq    = hd.seq;
                status = GET_TOOBIG; 
                goto trackit;    /*changed 960612:ldd*/
             }
        /* copy message by chunks to caller's address */
             nwrap = ir + hd.size - shm->keymax;
             if ( nwrap <= 0 )
             {
                memcpy( (void *) msg, (void *) &ring[ir], hd.size );
             }
             else
             {
                nfill = hd.size - nwrap;
                memcpy( (void *) &msg[0],     (void *) &ring[ir], nfill );
                memcpy( (void *) &msg[nfill], (void *) &ring[0],  nwrap );
             }
        /* see if we got lapped by tport_copyto or tport_putmsg while copying msg */
        /* if we did, go back and try to get a msg cleanly          */
             keyold = shm->keyold; 
             if ( keyold >= keyget ) 
             {
                keyget = keyold;
		lapped = 1; 
                goto nextmsg;
             }

        /* set other returned variables */
            *logo   = hd.logo;
            *length = hd.size;
            *seq    = hd.seq;

trackit:
        /* find logo in tracked list */
             for ( it=0 ; it < nlogo ; it++ )  
             {
                if ( region->key    != trak[it].memkey      )  continue;
                if ( hd.logo.type   != trak[it].logo.type   )  continue;
                if ( hd.logo.mod    != trak[it].logo.mod    )  continue;
                if ( hd.logo.instid != trak[it].logo.instid )  continue;
                /* activate sequence tracking if 1st msg */  
                if ( !trak[it].active )
                {
                    trak[it].seq    = hd.seq;
                    trak[it].active = 1;
                }
                goto sequence;
             }
        /* new logo, track it if there's room */   
             if ( nlogo == NTRACK_GET ) 
             {
                fprintf( stdout, 
                     "ERROR: tport_copyfrom; exceeded NTRACK_GET\n"); 
                if( status != GET_TOOBIG ) status = GET_NOTRACK; /*changed 960612:ldd*/
                goto wrapup;
             }
             it = nlogo;
             trak[it].memkey = region->key;
             trak[it].logo   = hd.logo;  
             trak[it].seq    = hd.seq;
             trak[it].active = 1;      /*960618:ldd*/
             nlogo++; 
            
sequence:
        /* check if sequence #'s match; update sequence # */
             if ( status == GET_TOOBIG   )  goto wrapup; /*added 960612:ldd*/     
             if ( hd.seq != trak[it].seq ) 
             {
       		if (lapped)  status = GET_MISS_LAPPED;
                else         status = GET_MISS_SEQGAP; 
                trak[it].seq = hd.seq;
             }
             trak[it].seq++;
 
        /* Ok, we're finished grabbing this one */
             goto wrapup;

          } /* end if of logo & getlogo match */
       }    /* end for over getlogo */
   }        /* end while over ring */

/**** If you got here, there were no messages of requested logo(s) ****/

   status = GET_NONE;

/**** update outpointer (->msg after retrieved one) for all requested logos ****/

wrapup:
   for ( it=0 ; it < nlogo ; it++ )  /* for all message logos we're tracking */
   {
       if ( trak[it].memkey != region->key ) continue;
       for ( j=0 ; j < nget ; j++ )  /* for all requested message logos */
       {
          if((getlogo[j].type   == trak[it].logo.type   || getlogo[j].type==WILD) &&
             (getlogo[j].mod    == trak[it].logo.mod    || getlogo[j].mod==WILD)  &&
             (getlogo[j].instid == trak[it].logo.instid || getlogo[j].instid==WILD) )
          {
             trak[it].keyout = keyget;
          }
       }
    }

   return( status );     

}


/*********************** function tport_copyto ***********************/
/*           Puts a message into a shared memory region. 	     */
/*    Preserves the sequence number (passed as argument) as the      */
/*                transport layer sequence number                    */
/*********************************************************************/

int tport_copyto( SHM_INFO    *region,  /* info structure for memory region   */
		  MSG_LOGO    *putlogo, /* type,module,instid of incoming msg */
		  long         length,  /* size of incoming message           */
		  char        *msg,     /* pointer to incoming message        */
		  unsigned char seq )   /* preserve as sequence# in TPORT_HEAD*/
{
   struct sembuf     sops;             /* semaphore operations; 
                                             changed to non-static 980424:ldd */
   SHM_HEAD         *shm;              /* pointer to start of memory region   */
   char             *ring;             /* pointer to ring part of memory      */
   long              ir;               /* index into memory ring              */
   long              nfill;            /* # bytes from ir to ring's last-byte */
   long              nwrap;            /* # bytes to wrap to front of ring    */
   TPORT_HEAD        hd;               /* transport layer header to put       */
   char             *h;                /* pointer to transport layer header   */
   TPORT_HEAD        old;              /* transport header of oldest msg      */
   char             *o;                /* pointer to oldest transport header  */
   int j;
   int res;
   int retval = PUT_OK;                /* return value for this function      */

/**** Initialize semaphore controls ****/

   sops.sem_num = 0;
   sops.sem_op  = 0;
   sops.sem_flg = 0;   

/**** Set up pointers for shared memory, etc. ****/

   shm  = region->addr;
   ring = (char *) shm + sizeof(SHM_HEAD);
   h    = (char *) (&hd);
   o    = (char *) (&old);

/**** First, see if the incoming message will fit in the memory region ****/

   if ( length + sizeof(TPORT_HEAD) > shm->keymax )
   {
      fprintf( stdout, 
              "ERROR: tport_copyto; message too large (%ld) for Region %ld\n",
               length, region->key);
      return( PUT_TOOBIG ); 
   }
   
/**** Block signals that Earthworm uses to kill processes and threads ****/

   tport_blocksigs();

/**** Change semaphore to let others know you're using memory ****/

   sops.sem_op = SHM_INUSE;
   res = semop( region->sid, &sops, 1 );   
   if (res == -1)
      tport_syserr( "tport_copyto semop ->inuse", region->key );          

/**** Store everything you need in the transport header ****/

   hd.start = FIRST_BYTE;
   hd.size  = length;
   hd.logo  = *putlogo;
   hd.seq   = seq;

/**** First see if keyin will wrap; if so, reset both keyin and keyold ****/

   if ( shm->keyin + sizeof(TPORT_HEAD) + length  <  shm->keyold )
   {
       shm->keyin  = shm->keyin  % shm->keymax;
       shm->keyold = shm->keyold % shm->keymax;
       if ( shm->keyin <= shm->keyold ) shm->keyin += shm->keymax;
     /*fprintf( stdout, 
               "NOTICE: tport_copyto; keyin wrapped & reset; Region %ld\n",
                region->key );*/
   }

/**** Then see if there's enough room for new message in shared memory ****/
/****      If not, "delete" oldest messages until there's room         ****/

   while( shm->keyin + sizeof(TPORT_HEAD) + length - shm->keyold > shm->keymax )
   {
      ir = shm->keyold % shm->keymax;
      if ( ring[ir] != FIRST_BYTE )
      {
          fprintf( stdout, 
                  "ERROR: tport_copyto; keyold not at FIRST_BYTE, Region %ld\n",
                   region->key );
          retval = TPORT_FATAL;
          goto release_semaphore; 
      }
      for ( j=0 ; j < sizeof(TPORT_HEAD) ; j++ )
      {
         if ( ir >= shm->keymax )   ir -= shm->keymax;
         o[j] = ring[ir++];
      } 
      shm->keyold += sizeof(TPORT_HEAD) + old.size;
   }

/**** Now copy transport header into shared memory by chunks... ****/

   ir = shm->keyin % shm->keymax;
   nwrap = ir + sizeof(TPORT_HEAD) - shm->keymax;
   if ( nwrap <= 0 )
   {
         memcpy( (void *) &ring[ir], (void *) h, sizeof(TPORT_HEAD) );
   }
   else
   {
         nfill = sizeof(TPORT_HEAD) - nwrap;
         memcpy( (void *) &ring[ir], (void *) &h[0],     nfill );
         memcpy( (void *) &ring[0],  (void *) &h[nfill], nwrap );
   }
   ir += sizeof(TPORT_HEAD);
   if ( ir >= shm->keymax )  ir -= shm->keymax;

/**** ...and copy message into shared memory by chunks ****/

   nwrap = ir + length - shm->keymax;
   if ( nwrap <= 0 )
   {
         memcpy( (void *) &ring[ir], (void *) msg, length );
   }
   else
   {
         nfill = length - nwrap;
         memcpy( (void *) &ring[ir], (void *) &msg[0],     nfill );
         memcpy( (void *) &ring[0],  (void *) &msg[nfill], nwrap );
   }
   shm->keyin += sizeof(TPORT_HEAD) + length;

/**** Finished with shared memory, let others know via semaphore ****/

release_semaphore:
   sops.sem_op = SHM_FREE;
   res = semop( region->sid, &sops, 1 );  
   if (res == -1)
      tport_syserr( "tport_copyto semop ->free", region->key ); 

/**** Release signals that Earthworm uses to kill processes and threads ****/

   tport_releasesigs();

   if( retval == TPORT_FATAL ) exit( 1 );
   return( retval ); 
}


/************************* tport_buferror ***************************/
/*  Build an error message and put it in the public memory region   */
/********************************************************************/
void tport_buferror( short  ierr, 	/* 2-byte error word       */
		     char  *note  )	/* string describing error */
{
	MSG_LOGO    logo;
	char	    msg[256];
	int         res;
	long	    size;
	time_t	    t;
     
	logo.instid = MyInstid;
        logo.mod    = MyModuleId;
        logo.type   = TypeError;

        time( &t );
	sprintf( msg, "%ld %hd %s\n", t, ierr, note );
	size = strlen( msg );   /* don't include the null byte in the message */

 	if ( tport_putmsg( (SHM_INFO *) PubRegion, &logo, size, msg ) != PUT_OK )
	{
	    printf("tport_bufthr:  Error sending error:%hd for module:%d.\n", 
		    ierr, MyModuleId );
	}     
	return;
}


/************************ function tport_syserr **********************/
/*                 Print a system error and terminate.               */
/*********************************************************************/

void tport_syserr( char *msg,   /* message to print (which routine had an error) */
		   long  key )  /* identifies which memory region had the error  */
{

   fprintf( stdout, "ERROR: %s (%d", msg, errno);

   fprintf( stdout,"; %s) Region: %ld\n", strerror(errno), key );

   exit( 1 );
}


/********************* function tport_blocksigs **********************/
/*          Block certain signals to protect important code          */
/*********************************************************************/

void tport_blocksigs( void )
{
   sighold( SIGTERM ); /* in startstop.c, causes a process to exit */
   sighold( SIGUSR1 ); /* in threads_ew.c, causes a thread to exit */
   return;
}


/********************* function tport_releasesigs ********************/
/* Re-enable signals that were previously blocked in tport_blocksigs */
/*********************************************************************/

void tport_releasesigs( void )
{
   sigrelse( SIGTERM );
   sigrelse( SIGUSR1 );
   return;
}

/******************* function tport_createFlag **********************/
/*        Create the shared memory flag & its semaphore,            */
/*           attach to it and initialize header values.             */  
/********************************************************************/

void tport_createFlag()
{
   SHM_FLAG *faddr;
   
   if ( Flag_Init == 0 )
   		return;
   
   shm_flag_key = GetKeyWithDefault( SHM_FLAG_RING, SHM_FLAG_DEFAULT_KEY );

   tport_create( &smf_region, sizeof(SHM_FLAG), shm_flag_key );
   
   faddr = (SHM_FLAG *)smf_region.addr;
   
   faddr->nPidsToDie = faddr->nPids = 0;
   
   Flag_Init = 0;
}

/****************** function tport_destroyFlag **********************/
/*                Destroy the shared memory flag.                   */
/********************************************************************/

void  tport_destroyFlag()
{
   tport_destroy( &smf_region );
}