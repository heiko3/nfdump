/*
 *  Copyright (c) 2022, Peter Haag
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright notice,
 *     this list of conditions and the following disclaimer in the documentation
 *     and/or other materials provided with the distribution.
 *   * Neither the name of the author nor the names of its contributors may be
 *     used to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 *  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 *  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 *  ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 *  LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 *  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 *  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 *  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 *  CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 *  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include "pcapdump.h"

#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "flist.h"
#include "nffile.h"
#include "packet_pcap.h"
#include "queue.h"
#include "util.h"

#define PCAP_TMP "pcap.current"
#define MAXBUFFERS 8

static char pcap_dumpfile[MAXPATHLEN];

/*
 * Function prototypes
 */
static int OpenDumpFile(flushParam_t *param);

static int CloseDumpFile(flushParam_t *param, time_t t_start);

/*
 * Functions
 */

static int OpenDumpFile(flushParam_t *param) {
    dbg_printf("OpenDumpFile()\n");
    FILE *pFile = fopen(pcap_dumpfile, "wb");
    if (!pFile) {
        LogError("fopen() error in %s line %d: %s", __FILE__, __LINE__, strerror(errno));
        return -1;
    }
    param->pd = pcap_dump_fopen(param->pcap_dev, pFile);
    if (!param->pd) {
        LogError("Fatal: pcap_dump_open() failed for file '%s': %s", pcap_dumpfile, pcap_geterr(param->pcap_dev));
        return -1;
    }

    fflush(pFile);
    param->pfd = fileno((FILE *)pFile);
    return 0;

}  // End of OpenDumpFile

static int CloseDumpFile(flushParam_t *param, time_t t_start) {
    struct tm *when;
    int err;
    char datefile[MAXPATHLEN];

    if (param->pd == NULL) return 1;

    pcap_dump_close(param->pd);
    param->pd = NULL;
    param->pfd = 0;

    dbg_printf("CloseDumpFile()\n");
    when = localtime(&t_start);
    char fmt[16];
    strftime(fmt, sizeof(fmt), param->extensionFormat, when);
    if (param->subdir_index) {
        char *subdir = GetSubDir(when);
        char error[256];
        if (!subdir || !SetupSubDir(param->archivedir, subdir, error, 255)) {
            LogError("Create subdir failed: %s", error);
            subdir = "";
        }

        snprintf(datefile, MAXPATHLEN - 1, "%s/%s/pcap.%s", param->archivedir, subdir, fmt);
    } else {
        snprintf(datefile, MAXPATHLEN - 1, "%s/pcap.%s", param->archivedir, fmt);
    }

    dbg_printf("CloseDumpFile() %s -> %s\n", pcap_dumpfile, datefile);
    err = rename(pcap_dumpfile, datefile);
    if (err) {
        LogError("rename() failed: %s", strerror(errno));
    }

    return 0;

}  // End of CloseDumpFile

int InitBufferQueues(flushParam_t *flushParam) {
    flushParam->bufferQueue = queue_init(MAXBUFFERS);
    flushParam->flushQueue = queue_init(MAXBUFFERS);
    if (!flushParam->bufferQueue || !flushParam->flushQueue) {
        LogError("Init buffer queues failed");
        return -1;
    }
    for (int i = 0; i < MAXBUFFERS; i++) {
        packetBuffer_t *packetBuffer = calloc(1, sizeof(packetBuffer_t));
        if (!packetBuffer) {
            LogError("calloc() error in %s line %d: %s\n", __FILE__, __LINE__, strerror(errno));
            return -1;
        }
        packetBuffer->buffer = malloc(BUFFSIZE);
        if (!packetBuffer->buffer) {
            LogError("malloc() error in %s line %d: %s\n", __FILE__, __LINE__, strerror(errno));
            return -1;
        }
        queue_push(flushParam->bufferQueue, (void *)packetBuffer);
    }

    return 0;

}  // End of InitBufferQueues

void __attribute__((noreturn)) * flush_thread(void *args) {
    flushParam_t *flushParam = (flushParam_t *)args;

    snprintf(pcap_dumpfile, MAXPATHLEN, "%s/%s-%i", flushParam->archivedir, PCAP_TMP, getpid());
    pcap_dumpfile[MAXPATHLEN - 1] = '\0';

    while (1) {
        packetBuffer_t *packetBuffer = queue_pop(flushParam->flushQueue);
        if (packetBuffer == QUEUE_CLOSED) {
            break;
        }
        dbg_printf("flush_thread() next buffer: %zu\n", packetBuffer->bufferSize);
        time_t timeStamp = packetBuffer->timeStamp;
        if (packetBuffer->bufferSize) {
            if ((flushParam->pd == NULL) && (OpenDumpFile(flushParam) < 0)) {
                // tell parent, we are dying
                pthread_kill(flushParam->parent, SIGUSR1);
                pthread_exit("OpenDumpFile failed.");
                /* NOTREACHED */
            }
            dbg_printf("flush_thread() flush buffer\n");
            if (write(flushParam->pfd, packetBuffer->buffer, packetBuffer->bufferSize) <= 0) {
                LogError("write() error in %s line %d: %s\n", __FILE__, __LINE__, strerror(errno));
            }

            // return buffer
            packetBuffer->bufferSize = 0;
            queue_push(flushParam->bufferQueue, packetBuffer);
        }
        if (timeStamp) {
            // rotate file
            dbg_printf("flush_thread() CloseDumpFile\n");
            if (CloseDumpFile(flushParam, timeStamp) < 0) {
                // tell parent, we are dying
                pthread_kill(flushParam->parent, SIGUSR1);
                pthread_exit("CloseDumpFile failed.");
                /* NOTREACHED */
            }
            packetBuffer->timeStamp = 0;
        }
    }

    pthread_exit("ok");
    /* NOTREACHED */

}  // End of flush_thread
