/* -*- mode: C; c-basic-offset: 4 -*- */
/* ex: set shiftwidth=4 tabstop=4 expandtab: */
/*
 * Copyright (c) 2008-2011, Georgia Tech Research Corporation
 * All rights reserved.
 *
 * Author(s): Neil T. Dantam <ntd@gatech.edu>
 * Georgia Tech Humanoid Robotics Lab
 * Under Direction of Prof. Mike Stilman <mstilman@cc.gatech.edu>
 *
 *
 * This file is provided under the following "BSD-style" License:
 *
 *
 *   Redistribution and use in source and binary forms, with or
 *   without modification, are permitted provided that the following
 *   conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *
 *   * Redistributions in binary form must reproduce the above
 *     copyright notice, this list of conditions and the following
 *     disclaimer in the documentation and/or other materials provided
 *     with the distribution.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND
 *   CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,
 *   INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 *   MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 *   DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
 *   CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF
 *   USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 *   AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 *   LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 *   ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *   POSSIBILITY OF SUCH DAMAGE.
 *
 */


/** \file ach.c
 *  \author Neil T. Dantam
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <time.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#include <unistd.h>
#include <ctype.h>
#include <stdbool.h>
#include <sys/stat.h>

#include <string.h>
#include <inttypes.h>

#include "ach.h"

/* verbosity output levels */
/*
//#define WARN 0  ///< verbosity level for warnings
//#define INFO 1  ///< verbosity level for info messages
//#define DEBUG 2 ///< verbosity level for debug messages

// print a debug messages at some level
//#define DEBUGF(level, fmt, a... )\
//(((args.verbosity) >= level )?fprintf( stderr, (fmt), ## a ) : 0);
*/

/** macro to print debug messages */
#define DEBUGF(fmt, a... )                      \
    fprintf(stderr, (fmt), ## a )

/** Call perror() when debugging */
#define DEBUG_PERROR(a)    perror(a)

/** macro to do things when debugging */
#define IFDEBUG( x ) (x)


size_t ach_channel_size = sizeof(ach_channel_t);

size_t ach_attr_size = sizeof(ach_attr_t);



static size_t oldest_index_i( ach_header_t *shm ) {
    return (shm->index_head + shm->index_free)%shm->index_cnt;
}

static size_t last_index_i( ach_header_t *shm ) {
    return (shm->index_head + shm->index_cnt -1)%shm->index_cnt;
}

const char *ach_result_to_string(ach_status_t result) {

    switch(result) {
    case ACH_OK: return "ACH_OK";
    case ACH_OVERFLOW: return "ACH_OVERFLOW";
    case ACH_INVALID_NAME: return "ACH_INVALID_NAME";
    case ACH_BAD_SHM_FILE: return "ACH_BAD_SHM_FILE";
    case ACH_FAILED_SYSCALL: return "ACH_FAILED_SYSCALL";
    case ACH_STALE_FRAMES: return "ACH_STALE_FRAMES";
    case ACH_MISSED_FRAME: return "ACH_MISSED_FRAME";
    case ACH_TIMEOUT: return "ACH_TIMEOUT";
    case ACH_CLOSED: return "ACH_CLOSED";
    case ACH_EEXIST: return "ACH_EEXIST";
    case ACH_ENOENT: return "ACH_ENOENT";
    case ACH_BUG: return "ACH_BUG";
    case ACH_EINVAL: return "ACH_EINVAL";
    case ACH_CORRUPT: return "ACH_CORRUPT";
    case ACH_BAD_HEADER: return "ACH_BAD_HEADER";
    case ACH_EACCES: return "ACH_EACCES";
    }
    return "UNKNOWN";

}

static enum ach_status
check_errno() {
    switch(errno) {
    case EEXIST: return ACH_EEXIST;
    case ENOENT: return ACH_ENOENT;
    case EACCES: return ACH_EACCES;
    default: return ACH_FAILED_SYSCALL;
    }
}

static enum ach_status
check_guards( ach_header_t *shm ) {
    if( ACH_SHM_MAGIC_NUM != shm->magic ||
        ACH_SHM_GUARD_HEADER_NUM != *ACH_SHM_GUARD_HEADER(shm) ||
        ACH_SHM_GUARD_INDEX_NUM != *ACH_SHM_GUARD_INDEX(shm) ||
        ACH_SHM_GUARD_DATA_NUM != *ACH_SHM_GUARD_DATA(shm)  )
    {
        return ACH_CORRUPT;
    } else {
        return ACH_OK;
    }
}


/* returns 0 if channel name is bad */
static int channel_name_ok( const char *name ) {
    size_t len;
    /* check size */
    if( (len = strlen( name )) >= ACH_CHAN_NAME_MAX )
        return 0;
    /* check hidden file */
    if( name[0] == '.' ) return 0;
    /* check bad characters */
    size_t i;
    for( i = 0; i < len; i ++ ) {
        if( ! ( isalnum( name[i] )
                || (name[i] == '-' )
                || (name[i] == '_' )
                || (name[i] == '.' ) ) )
            return 0;
    }
    return 1;
}

static enum ach_status
shmfile_for_channel_name( const char *name, char *buf, size_t n ) {
    if( n < ACH_CHAN_NAME_MAX + 16 ) return ACH_BUG;
    if( !channel_name_ok(name)   ) return ACH_INVALID_NAME;
    strcpy( buf, ACH_CHAN_NAME_PREFIX );
    strncat( buf, name, ACH_CHAN_NAME_MAX );
    return ACH_OK;
}

/** Opens shm file descriptor for a channel.
    \pre name is a valid channel name
*/
static int fd_for_channel_name( const char *name, int oflag ) {
    char shm_name[ACH_CHAN_NAME_MAX + 16];
    int r = shmfile_for_channel_name( name, shm_name, sizeof(shm_name) );
    if( 0 != r ) return ACH_BUG;
    int fd;
    int i = 0;
    do {
        fd = shm_open( shm_name, O_RDWR | oflag, 0666 );
    }while( -1 == fd && EINTR == errno && i++ < ACH_INTR_RETRY);
    return fd;
}



/*! \page synchronization Synchronization
 *
 * Synchronization currently uses a simple mutex+condition variable
 * around the whole shared memory block
 *
 * Some idea for more complicated synchronization:
 *
 * Our synchronization for shared memory could work roughly like a
 * read-write lock with one one additional feature.  A reader may
 * choose to block until the next write is performed.
 *
 * This behavior could be implemented with a a state variable, a
 * mutex, two condition variables, and three counters.  One condition
 * variable is for writers, and the other for readers.  We count
 * active readers, waiting readers, and waiting writers.  If a writer
 * is waiting, readers will block until it finishes.
 *
 * It may be possible to make these locks run faster by doing some
 * CASs on the state word to handle the uncontended case.  Of course,
 * figuring out how to make this all lock free would really be
 * ideal...
 *
 *  \bug synchronization should be robust against processes terminating
 *
 * Mostly Lock Free Synchronization:
 * - Have a single word atomic sync variable
 * - High order bits are counts of writers, lower bits are counts of readers
 * - Fast path twiddles the counts.  Slow path deals with a mutex and cond-var.
 * - downside: maybe no way for priority inheritance to happen...
 *
 * Other Fancy things:
 * - Use futexes for waiting readers/writers
 * - Use eventfd to signal new data
 */


static enum ach_status
rdlock_wait( ach_header_t *shm, ach_channel_t *chan,
             const struct timespec *abstime ) {

    int r;
    r = pthread_mutex_lock( & shm->sync.mutex );
    assert( 0 == r );
    assert( 0 == shm->sync.dirty );
    /* if chan is passed, we wait for new data *
     * otherwise just return holding the lock  */
    while( chan &&
           chan->seq_num == shm->last_seq ) {

        if( abstime ) { /* timed wait */
            r = pthread_cond_timedwait( &shm->sync.cond,  &shm->sync.mutex, abstime );
            /* check for timeout */
            if( ETIMEDOUT == r ){
                pthread_mutex_unlock( &shm->sync.mutex );
                return ACH_TIMEOUT;
            }
        } else { /* wait forever */
            r = pthread_cond_wait( &shm->sync.cond,  &shm->sync.mutex );
        }
    }
    return ACH_OK;
}

static void rdlock( ach_header_t *shm ) {
    rdlock_wait( shm, NULL, NULL );
}

static void unrdlock( ach_header_t *shm ) {
    int r;
    assert( 0 == shm->sync.dirty );
    r = pthread_mutex_unlock( & shm->sync.mutex );
    assert( 0 == r );
}

static void wrlock( ach_header_t *shm ) {
    int r = pthread_mutex_lock( & shm->sync.mutex );
    assert( 0 == shm->sync.dirty );
    shm->sync.dirty = 1;
    assert( 0 == r );
}

static void unwrlock( ach_header_t *shm ) {
    int r;

    /* mark clean */
    assert( 1 == shm->sync.dirty );
    shm->sync.dirty = 0;

    /* unlock */
    r = pthread_mutex_unlock( & shm->sync.mutex );
    assert( 0 == r );

    /* broadcast to wake up waiting readers */
    r = pthread_cond_broadcast( & shm->sync.cond );
    assert( 0 == r );

}


void ach_create_attr_init( ach_create_attr_t *attr ) {
    memset( attr, 0, sizeof( ach_create_attr_t ) );
}

enum ach_status
ach_create( const char *channel_name,
            size_t frame_cnt, size_t frame_size,
            ach_create_attr_t *attr) {
    ach_header_t *shm;
    int fd;
    size_t len;
    /* fixme: truncate */
    /* open shm */
    {
        len = sizeof( ach_header_t) +
            frame_cnt*sizeof( ach_index_t ) +
            frame_cnt*frame_size +
            3*sizeof(uint64_t);

        if( attr && attr->map_anon ) {
            /* anonymous (heap) */
            shm = (ach_header_t *) malloc( len );
            fd = -1;
        }else {
            int oflag = O_EXCL | O_CREAT;
            /* shm */
            if( ! channel_name_ok( channel_name ) )
                return ACH_INVALID_NAME;
            if( attr ) {
                if( attr->truncate ) oflag &= ~O_EXCL;
            }
            if( (fd = fd_for_channel_name( channel_name, oflag )) < 0 ) {
                return check_errno();;
            }

            { /* make file proper size */
                /* FreeBSD needs ftruncate before mmap, Linux can do either order */
                int r;
                int i = 0;
                do {
                    r = ftruncate( fd, (off_t) len );
                }while(-1 == r && EINTR == errno && i++ < ACH_INTR_RETRY);
                if( -1 == r ) {
                    DEBUG_PERROR( "ftruncate");
                    return ACH_FAILED_SYSCALL;
                }
            }

            /* mmap */
            if( (shm = (ach_header_t *)mmap( NULL, len, PROT_READ|PROT_WRITE,
                                             MAP_SHARED, fd, 0) )
                == MAP_FAILED ) {
                DEBUG_PERROR("mmap");
                DEBUGF("mmap failed %s, len: %"PRIuPTR", fd: %d\n", strerror(errno), len, fd);
                return ACH_FAILED_SYSCALL;
            }

        }

        memset( shm, 0, len );
        shm->len = len;
    }

    { /* initialize synchronization */
        { /* initialize condition variables */
            int r;
            pthread_condattr_t cond_attr;
            if( (r = pthread_condattr_init(&cond_attr)) ) {
                DEBUG_PERROR("pthread_condattr_init");
                return ACH_FAILED_SYSCALL;
            }
            /* Process Shared */
            if( ! (attr && attr->map_anon) ) {
                /* Set shared if not anonymous mapping
                   Default will be private. */
                if( (r = pthread_condattr_setpshared(&cond_attr, PTHREAD_PROCESS_SHARED)) ) {
                    DEBUG_PERROR("pthread_condattr_setpshared");
                    return ACH_FAILED_SYSCALL;
                }
            }
            /* Clock */
            if( attr && attr->set_clock ) {
                if( (r = pthread_condattr_setclock(&cond_attr, attr->clock)) ) {
                    DEBUG_PERROR("pthread_condattr_setclock");
                    return ACH_FAILED_SYSCALL;
                }
            } else {
                if( (r = pthread_condattr_setclock(&cond_attr, ACH_DEFAULT_CLOCK)) ) {
                    DEBUG_PERROR("pthread_condattr_setclock");
                    return ACH_FAILED_SYSCALL;
                }
            }

            if( (r = pthread_cond_init(&shm->sync.cond, &cond_attr)) ) {
                DEBUG_PERROR("pthread_cond_init");
                return ACH_FAILED_SYSCALL;
            }

            if( (r = pthread_condattr_destroy(&cond_attr)) ) {
                DEBUG_PERROR("pthread_condattr_destroy");
                return ACH_FAILED_SYSCALL;
            }
        }
        { /* initialize mutex */
            int r;
            pthread_mutexattr_t mutex_attr;
            if( (r = pthread_mutexattr_init(&mutex_attr)) ) {
                DEBUG_PERROR("pthread_mutexattr_init");
                return ACH_FAILED_SYSCALL;
            }
            if( (r = pthread_mutexattr_setpshared(&mutex_attr,
                                                  PTHREAD_PROCESS_SHARED)) ) {
                DEBUG_PERROR("pthread_mutexattr_setpshared");
                return ACH_FAILED_SYSCALL;
            }
            /* Error Checking Mutex */
#ifdef PTHREAD_MUTEX_ERRORCHECK_NP
            if( (r = pthread_mutexattr_settype(&mutex_attr,
                                               PTHREAD_MUTEX_ERRORCHECK_NP)) ) {
                DEBUG_PERROR("pthread_mutexattr_settype");
                return ACH_FAILED_SYSCALL;
            }
#endif
            /* Priority Inheritance Mutex */
#ifdef PTHREAD_PRIO_INHERIT
            if( (r = pthread_mutexattr_setprotocol(&mutex_attr,
                                                   PTHREAD_PRIO_INHERIT)) ) {
                DEBUG_PERROR("pthread_mutexattr_setprotocol");
                return ACH_FAILED_SYSCALL;
            }
#endif

            if( (r = pthread_mutex_init(&shm->sync.mutex, &mutex_attr)) ) {
                DEBUG_PERROR("pthread_mutexattr_init");
                return ACH_FAILED_SYSCALL;
            }

            if( (r = pthread_mutexattr_destroy(&mutex_attr)) ) {
                DEBUG_PERROR("pthread_mutexattr_destroy");
                return ACH_FAILED_SYSCALL;
            }
        }
    }
    /* initialize name */
    strncpy( shm->name, channel_name, ACH_CHAN_NAME_MAX );
    /* initialize counts */
    shm->index_cnt = frame_cnt;
    shm->index_head = 0;
    shm->index_free = frame_cnt;
    shm->data_head = 0;
    shm->data_free = frame_cnt * frame_size;
    shm->data_size = frame_cnt * frame_size;
    assert( sizeof( ach_header_t ) +
            shm->index_free * sizeof( ach_index_t ) +
            shm->data_free + 3*sizeof(uint64_t) ==  len );

    *ACH_SHM_GUARD_HEADER(shm) = ACH_SHM_GUARD_HEADER_NUM;
    *ACH_SHM_GUARD_INDEX(shm) = ACH_SHM_GUARD_INDEX_NUM;
    *ACH_SHM_GUARD_DATA(shm) = ACH_SHM_GUARD_DATA_NUM;
    shm->magic = ACH_SHM_MAGIC_NUM;

    if( attr && attr->map_anon ) {
        attr->shm = shm;
    } else {
        int r;
        /* remove mapping */
        r = munmap(shm, len);
        if( 0 != r ){
            DEBUG_PERROR("munmap");
            return ACH_FAILED_SYSCALL;
        }
        /* close file */
        int i = 0;
        do {
            IFDEBUG( i ? DEBUGF("Retrying close()\n"):0 );
            r = close(fd);
        }while( -1 == r && EINTR == errno && i++ < ACH_INTR_RETRY );
        if( -1 == r ){
            DEBUG_PERROR("close");
            return ACH_FAILED_SYSCALL;
        }
    }
    return ACH_OK;
}

enum ach_status
ach_open( ach_channel_t *chan, const char *channel_name,
          ach_attr_t *attr ) {
    ach_header_t * shm;
    size_t len;
    int fd = -1;

    if( attr ) memcpy( &chan->attr, attr, sizeof(chan->attr) );
    else memset( &chan->attr, 0, sizeof(chan->attr) );

    if( attr && attr->map_anon ) {
        shm = attr->shm;
        len = sizeof(ach_header_t) + sizeof(ach_index_t)*shm->index_cnt + shm->data_size;
    }else {
        if( ! channel_name_ok( channel_name ) )
            return ACH_INVALID_NAME;
        /* open shm */
        if( ! channel_name_ok( channel_name ) ) return ACH_INVALID_NAME;
        if( (fd = fd_for_channel_name( channel_name, 0 )) < 0 ) {
            return check_errno();
        }
        if( (shm = (ach_header_t*) mmap (NULL, sizeof(ach_header_t),
                                         PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0ul) )
            == MAP_FAILED )
            return ACH_FAILED_SYSCALL;
        if( ACH_SHM_MAGIC_NUM != shm->magic )
            return ACH_BAD_SHM_FILE;

        /* calculate mmaping size */
        len = sizeof(ach_header_t) + sizeof(ach_index_t)*shm->index_cnt + shm->data_size;

        /* remap */
        if( -1 ==  munmap( shm, sizeof(ach_header_t) ) )
            return check_errno();

        if( (shm = (ach_header_t*) mmap( NULL, len, PROT_READ|PROT_WRITE,
                                         MAP_SHARED, fd, 0ul) )
            == MAP_FAILED )
            return check_errno();
    }

    /* Check guard bytes */
    {
        enum ach_status r = check_guards(shm);
        if( ACH_OK != r ) return r;
    }

    /* initialize struct */
    chan->fd = fd;
    chan->len = len;
    chan->shm = shm;
    chan->seq_num = 0;
    chan->next_index = 1;

    return ACH_OK;
}


/** Copies frame pointed to by index entry at index_offset.

    \pre hold read lock on the channel

    \pre on success, buf holds the frame seq_num and next_index fields
    are incremented. The variable pointed to by size_written holds the
    number of bytes written to buf (0 on failure).
*/
static enum ach_status
ach_get_from_offset( ach_channel_t *chan, size_t index_offset,
                     char *buf, size_t size, size_t *frame_size ) {
    ach_header_t *shm = chan->shm;
    assert( index_offset < shm->index_cnt );
    ach_index_t *idx = ACH_SHM_INDEX(shm) + index_offset;
    /* assert( idx->size ); */
    assert( idx->seq_num );
    assert( idx->offset < shm->data_size );
    /* check idx */
    if( chan->seq_num > idx->seq_num ) {
        fprintf(stderr,
                "ach bug: chan->seq_num (%"PRIu64") > idx->seq_num (%"PRIu64")\n"
                "ach bug: index offset: %"PRIuPTR"\n",
                chan->seq_num, idx->seq_num,
                index_offset );
        return ACH_BUG;
    }

    if(  idx->size > size ) {
        /* buffer overflow */
        *frame_size = idx->size;
        return ACH_OVERFLOW;
    } else {
        /* good to copy */
        uint8_t *data_buf = ACH_SHM_DATA(shm);
        if( idx->offset + idx->size < shm->data_size ) {
            /* simple memcpy */
            memcpy( (uint8_t*)buf, data_buf + idx->offset, idx->size );
        }else {
            /* wraparound memcpy */
            size_t end_cnt = shm->data_size - idx->offset;
            memcpy( (uint8_t*)buf, data_buf + idx->offset, end_cnt );
            memcpy( (uint8_t*)buf + end_cnt, data_buf, idx->size - end_cnt );
        }
        *frame_size = idx->size;
        chan->seq_num = idx->seq_num;
        chan->next_index = (index_offset + 1) % shm->index_cnt;
        return ACH_OK;
    }
}

enum ach_status
ach_get( ach_channel_t *chan, void *buf, size_t size,
         size_t *frame_size,
         const struct timespec *ACH_RESTRICT abstime,
         int options ) {
    ach_header_t *shm = chan->shm;
    ach_index_t *index_ar = ACH_SHM_INDEX(shm);

    /* Check guard bytes */
    {
        enum ach_status r = check_guards(shm);
        if( ACH_OK != r ) return r;
    }

    const bool o_wait = options & ACH_O_WAIT;
    const bool o_last = options & ACH_O_LAST;
    const bool o_copy = options & ACH_O_COPY;

    /* take read lock */
    if( o_wait ) {
        enum ach_status r;
        if( ACH_OK != (r = rdlock_wait( shm, chan, abstime ) ) ) {
            return r;
        }
    } else { rdlock( shm ); }

    assert( chan->seq_num <= shm->last_seq );

    enum ach_status retval = ACH_BUG;
    bool missed_frame = 0;

    /* get the data */
    if( (chan->seq_num == shm->last_seq && !o_copy) || 0 == shm->last_seq ) {
        /* no entries */
        assert(!o_wait);
        retval = ACH_STALE_FRAMES;
    } else {
        /* Compute the index to read */
        size_t read_index;
        if( o_last ) {
            /* normal case, get last */
            read_index = last_index_i(shm);
        } else if (!o_last &&
                   index_ar[chan->next_index].seq_num == chan->seq_num + 1) {
            /* normal case, get next */
            read_index = chan->next_index;
        } else {
            /* exception case, figure out which frame */
            if (chan->seq_num == shm->last_seq) {
                /* copy last */
                assert(o_copy);
                read_index = last_index_i(shm);
            } else {
                /* copy oldest */
                read_index = oldest_index_i(shm);
            }
        }

        if( index_ar[read_index].seq_num > chan->seq_num + 1 ) { missed_frame = 1; }

        /* read from the index */
        retval = ach_get_from_offset( chan, read_index, (char*)buf, size,
                                      frame_size );

        assert( index_ar[read_index].seq_num > 0 );
    }

    /* release read lock */
    unrdlock( shm );

    return (ACH_OK == retval && missed_frame) ? ACH_MISSED_FRAME : retval;
}


enum ach_status
ach_flush( ach_channel_t *chan ) {
    /*int r; */
    ach_header_t *shm = chan->shm;
    rdlock(shm);
    chan->seq_num = shm->last_seq;
    chan->next_index = shm->index_head;
    unrdlock(shm);
    return ACH_OK;
}


static void free_index(ach_header_t *shm, size_t i ) {
    ach_index_t *index_ar = ACH_SHM_INDEX(shm);

    assert( index_ar[i].seq_num ); /* only free used indices */
    assert( index_ar[i].size );    /* must have some data */
    assert( shm->index_free < shm->index_cnt ); /* must be some used index */

    shm->data_free += index_ar[i].size;
    shm->index_free ++;
    memset( &index_ar[i], 0, sizeof( ach_index_t ) );
}

enum ach_status
ach_put( ach_channel_t *chan, const void *buf, size_t len ) {
    if( 0 == len || NULL == buf || NULL == chan->shm ) {
        return ACH_EINVAL;
    }

    ach_header_t *shm = chan->shm;

    /* Check guard bytes */
    {
        enum ach_status r = check_guards(shm);
        if( ACH_OK != r ) return r;
    }

    if( shm->data_size < len ) {
        return ACH_OVERFLOW;
    }

    ach_index_t *index_ar = ACH_SHM_INDEX(shm);
    uint8_t *data_ar = ACH_SHM_DATA(shm);

    if( len > shm->data_size ) return ACH_OVERFLOW;

    /* take write lock */
    wrlock( shm );

    /* find next index entry */
    ach_index_t *idx = index_ar + shm->index_head;

    /* clear entry used by index */
    if( 0 == shm->index_free ) { free_index(shm,shm->index_head); }
    else { assert(0== index_ar[shm->index_head].seq_num);}

    assert( shm->index_free > 0 );

    /* clear overlapping entries */
    size_t i;
    for(i = (shm->index_head + shm->index_free) % shm->index_cnt;
        shm->data_free < len;
        i = (i + 1) % shm->index_cnt) {
        assert( i != shm->index_head );
        free_index(shm,i);
    }

    assert( shm->data_free >= len );

    /* copy buffer */
    if( shm->data_size - shm->data_head >= len ) {
        /* simply copy */
        memcpy( data_ar + shm->data_head, buf, len );
    } else {
        /* wraparound copy */
        size_t end_cnt = shm->data_size - shm->data_head;
        memcpy( data_ar + shm->data_head, buf, end_cnt);
        memcpy( data_ar, (uint8_t*)buf + end_cnt, len - end_cnt );
    }

    /* modify counts */
    shm->last_seq++;
    idx->seq_num = shm->last_seq;
    idx->size = len;
    idx->offset = shm->data_head;

    shm->data_head = (shm->data_head + len) % shm->data_size;
    shm->data_free -= len;
    shm->index_head = (shm->index_head + 1) % shm->index_cnt;
    shm->index_free --;

    assert( shm->index_free <= shm->index_cnt );
    assert( shm->data_free <= shm->data_size );
    assert( shm->last_seq > 0 );

    /* release write lock */
    unwrlock( shm );
    return ACH_OK;
}

enum ach_status
ach_close( ach_channel_t *chan ) {

    /* Check guard bytes */
    {
        enum ach_status r = check_guards(chan->shm);
        if( ACH_OK != r ) return r;
    }

    /* fprintf(stderr, "Closing\n"); */
    /* note the close in the channel */
    if( chan->attr.map_anon ) {
        /* FIXME: what to do here?? */
        ;
    } else {
        /* remove mapping */
        int r = munmap(chan->shm, chan->len);
        if( 0 != r ){
            DEBUGF("Failed to munmap channel\n");
            return ACH_FAILED_SYSCALL;
        }
        chan->shm = NULL;

        /* close file */
        int i = 0;
        do {
            IFDEBUG( i ? DEBUGF("Retrying close()\n"):0 );
            r = close(chan->fd);
        }while( -1 == r && EINTR == errno && i++ < ACH_INTR_RETRY );
        if( -1 == r ){
            DEBUGF("Failed to close() channel fd\n");
            return ACH_FAILED_SYSCALL;
        }
    }

    return ACH_OK;
}

void ach_dump( ach_header_t *shm ) {
    fprintf(stderr, "Magic: %x\n", shm->magic );
    fprintf(stderr, "len: %"PRIuPTR"\n", shm->len );
    fprintf(stderr, "data_size: %"PRIuPTR"\n", shm->data_size );
    fprintf(stderr, "data_head: %"PRIuPTR"\n", shm->data_head );
    fprintf(stderr, "data_free: %"PRIuPTR"\n", shm->data_free );
    fprintf(stderr, "index_head: %"PRIuPTR"\n", shm->index_head );
    fprintf(stderr, "index_free: %"PRIuPTR"\n", shm->index_free );
    fprintf(stderr, "last_seq: %"PRIu64"\n", shm->last_seq );
    fprintf(stderr, "head guard:  %"PRIx64"\n", * ACH_SHM_GUARD_HEADER(shm) );
    fprintf(stderr, "index guard: %"PRIx64"\n", * ACH_SHM_GUARD_INDEX(shm) );
    fprintf(stderr, "data guard:  %"PRIx64"\n", * ACH_SHM_GUARD_DATA(shm) );

    fprintf(stderr, "head seq:  %"PRIu64"\n",
            (ACH_SHM_INDEX(shm) +
             ((shm->index_head - 1 + shm->index_cnt) % shm->index_cnt)) -> seq_num );
    fprintf(stderr, "head size:  %"PRIuPTR"\n",
            (ACH_SHM_INDEX(shm) +
             ((shm->index_head - 1 + shm->index_cnt) % shm->index_cnt)) -> size );

}

void ach_attr_init( ach_attr_t *attr ) {
    memset( attr, 0, sizeof(ach_attr_t) );
}

enum ach_status
ach_chmod( ach_channel_t *chan, mode_t mode ) {
    return (0 == fchmod(chan->fd, mode)) ? ACH_OK : check_errno();;
}


enum ach_status
ach_unlink( const char *name ) {
    char shm_name[ACH_CHAN_NAME_MAX + 16];
    enum ach_status r = shmfile_for_channel_name( name, shm_name, sizeof(shm_name) );
    if( ACH_OK == r ) {
        /*r = shm_unlink(name); */
        int i = shm_unlink(shm_name);
        if( 0 == i ) {
            return  ACH_OK;
        } else {
            return check_errno();
        }
    } else {
        return r;
    }
}
