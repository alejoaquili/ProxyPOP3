/**
 * popv3nio.c  - controla el flujo de un proxy POPv3 (sockets no bloqueantes)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#include "popv3nio.h"
#include "buffer.h"
#include "logger.h"
#include "errorslib.h"
#include "Multiplexor.h"

#define BUFFER_SIZE 2048

#define N(x) (sizeof(x)/sizeof((x)[0]))

typedef enum popv3State {
    CONNECTING,
    AUTHORIZATION,
    TRANSACTION,
    UPDATE,
    COPY,
    DONE,
    ERROR,
} popv3State;

typedef struct copy {
    int *           fd;
    bufferADT       readBuffer;
    bufferADT       writeBuffer;
    fdInterest      duplex;
    struct copy *   other;
} copy;

typedef struct popv3 {
    popv3State state;
    int originFd;
    int clientFd;
    bufferADT readBuffer;
    bufferADT writeBuffer;
    copy clientCopy;
    copy originCopy;

    /** cantidad de referencias a este objeto. si es uno se debe destruir */
    unsigned references;
    /** siguiente en el pool */
    struct popv3 * next;
} popv3;

static void popv3Read(MultiplexorKey key);

static void popv3Write(MultiplexorKey key);

static void popv3Block(MultiplexorKey key);

static void popv3Close(MultiplexorKey key);

static void popv3Done(MultiplexorKey key);


static popv3 * newPopv3(int clientFd, int originFd, size_t bufferSize);

static void deletePopv3(popv3 * p);

static void copyInit(const unsigned state, MultiplexorKey key);

static fdInterest copyComputeInterests(MultiplexorADT mux, copy * d);

static copy * copyPtr(MultiplexorKey key);

static unsigned copyReadAndQueue(MultiplexorKey key);

static unsigned copyWrite(MultiplexorKey key);


static const eventHandler popv3Handler = {
    .read   = popv3Read,
    .write  = popv3Write,
    .block  = popv3Block,
    .close  = popv3Close,
};

#define ATTACHMENT(key) ( (struct popv3 *)(key)->data)

static void popv3Read(MultiplexorKey key) {
    logInfo("Starting to copy");
    copyInit(COPY, key);
    ATTACHMENT(key)->state = copyReadAndQueue(key);

    popv3State state = ATTACHMENT(key)->state;
    logDebug("state: %d", state);
    if(ERROR == state || DONE == state) {
        popv3Done(key);
    }
}

static void popv3Write(MultiplexorKey key) {
    copyInit(COPY, key);
    ATTACHMENT(key)->state = copyWrite(key);
    logInfo("Finishing copy");

    popv3State state = ATTACHMENT(key)->state;
    logDebug("state: %d", state);
    if(ERROR == state || DONE == state) {
        popv3Done(key);
    }
}

static void popv3Block(MultiplexorKey key) {

}

static void popv3Close(MultiplexorKey key) {
    deletePopv3(ATTACHMENT(key));
}

static void popv3Done(MultiplexorKey key) {
    logDebug("Estoy por desregistrar los fds en done");
    const int fds[] = {
        ATTACHMENT(key)->clientFd,
        ATTACHMENT(key)->originFd,
    };
    for(unsigned i = 0; i < N(fds); i++) {
        if(fds[i] != -1) {
            if(MUX_SUCCESS != unregisterFd(key->mux, fds[i])) {
                fail("Problem trying to unregister a fd: %d.", fds[i]);
            }
            close(fds[i]);
        }
    }
}

/**
 * Pool de `struct popv3', para ser reusados.
 *
 * Como tenemos un unico hilo que emite eventos no necesitamos barreras de
 * contención.
 */
static const unsigned  maxPool  = 50; // tamaño máximo
static unsigned        poolSize = 0;  // tamaño actual
static struct popv3  * pool      = 0;  // pool propiamente dicho

static popv3 * newPopv3(int clientFd, int originFd, size_t bufferSize) {
   
    struct popv3 * ret;
    if(pool == NULL) {
        ret = malloc(sizeof(*ret));
    } else {
        ret       = pool;
        pool      = pool->next;
        ret->next = 0;
    }
    if(ret == NULL) {
        goto finally;
    }
    memset(ret, 0x00, sizeof(*ret));

    ret->state = COPY;
    ret->clientFd = clientFd;
    ret->originFd = originFd;
    ret->readBuffer = createBuffer(BUFFER_SIZE);
    ret->writeBuffer = createBuffer(BUFFER_SIZE);
    
    ret->references = 1;
finally:
    return ret;
}

static void deletePopv3(popv3 * p) {
    if(p != NULL) {
        if(p->references == 1) {
            if(poolSize < maxPool) {
                p->next = pool;
                pool    = p;
                poolSize++;
            } else {
                free(p);
            }
        } else {
            p->references -= 1;
        }
    } 
}

void popv3PassiveAccept(MultiplexorKey key) {

    struct sockaddr_storage       client_addr;
    socklen_t                     client_addr_len = sizeof(client_addr);

    const int clientFd = accept(key->fd, (struct sockaddr*) &client_addr, &client_addr_len);
    if(clientFd == -1) {
        goto fail;
    }
    if(fdSetNIO(clientFd) == -1) {
        goto fail;
    }
    logInfo("Accepting new client from: ");
    
    ////connecting to server
    originServerAddr originAddr = *((originServerAddr *) key->data);
    int originFd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if(originFd == -1) {
        exit(1);
    }
    if(connect(originFd, (struct sockaddr *) &originAddr.ipv4, sizeof(originAddr.ipv4)) == -1) {
        logFatal("imposible conectar con el servidor origen");
        exit(1); //ACA DEBERIAMOS IR A UN ESTADO
    }
    /////

    popv3 * p = newPopv3(clientFd, originFd, BUFFER_SIZE);

    if(MUX_SUCCESS != registerFd(key->mux, clientFd, &popv3Handler, READ, p)) {
        goto fail;
    }
    if(MUX_SUCCESS != registerFd(key->mux, originFd, &popv3Handler, READ, p)) { //READ QUIERO SI ME DA MENSAJE DE +OK DEVOLVERLO
        goto fail;
    }
    logInfo("Connection established, client fd: %d, origin fd:%d", clientFd, originFd);

    return ;
fail:
    if(clientFd != -1) {
        close(clientFd);
    }
}

static void copyInit(const unsigned state, MultiplexorKey key) {
    struct popv3 * p = ATTACHMENT(key);

    copy * d        = &(p->clientCopy);
    d->fd           = &(p->clientFd);
    d->readBuffer   = p->readBuffer;
    d->writeBuffer  = p->writeBuffer;
    d->duplex       = READ | WRITE;
    d->other        = &(p->originCopy);

    d               = &(p->originCopy);
    d->fd           = &(p->originFd);
    d->readBuffer   = p->writeBuffer;
    d->writeBuffer  = p->readBuffer;
    d->duplex       = READ | WRITE;
    d->other        = &(p->clientCopy);
}

static fdInterest copyComputeInterests(MultiplexorADT mux, copy * d) {
    fdInterest ret = NO_INTEREST;
    if ((d->duplex & READ)  && canWrite(d->readBuffer))
        ret |= READ;
    if ((d->duplex & WRITE) && canRead(d->writeBuffer)) {
        ret |= WRITE;
    }
    logDebug("Interes computado: %d", ret);
    if(MUX_SUCCESS != setInterest(mux, *d->fd, ret))
        fail("Problem trying to set interest: %d,to multiplexor in copy, fd: %d.", ret, *d->fd);
    
    return ret;
}

static copy * copyPtr(MultiplexorKey key) {
    copy * d = &(ATTACHMENT(key)->clientCopy);

    if(*d->fd != key->fd)
        d = d->other;
    
    return  d;
}

static unsigned copyReadAndQueue(MultiplexorKey key) {
    copy * d = copyPtr(key);
    checkAreEquals(*d->fd, key->fd, "Copy destination and source have the same fd");

    size_t size;
    ssize_t n;
    bufferADT buffer = d->readBuffer;
    unsigned ret = COPY;

    uint8_t *ptr = getWritePtr(buffer, &size);
    n = recv(key->fd, ptr, size, 0);
    if(n <= 0) {
        logDebug("ALGUIEN CERRO LA CONEXION");
        shutdown(*d->fd, SHUT_RD);
        d->duplex &= ~READ;
        if(*d->other->fd != -1) {
            shutdown(*d->other->fd, SHUT_WR);
            d->other->duplex &= ~WRITE;
        }
    } else {
        updateWritePtr(buffer, n);
    }

    logDebug("duplex READ interest: %d", d->duplex);
    
    logMetric("Coppied from %s, total copied: %lu bytes", (*d->fd == ATTACHMENT(key)->clientFd)? "client to server" : "server to client", n);

    copyComputeInterests(key->mux, d);
    copyComputeInterests(key->mux, d->other);
    if(d->duplex == NO_INTEREST) {
        ret = DONE;
    }
    return ret;
}

static unsigned copyWrite(MultiplexorKey key) {
    copy * d = copyPtr(key);
    assert(*d->fd == key->fd);
    size_t size;
    ssize_t n;
    bufferADT buffer = d->writeBuffer;
    unsigned ret = COPY;

    uint8_t *ptr = getReadPtr(buffer, &size);
    n = send(key->fd, ptr, size, MSG_NOSIGNAL);
    if(n == -1) {
        shutdown(*d->fd, SHUT_WR);
        d->duplex &= ~WRITE;
        if(*d->other->fd != -1) {                       //shutdeteo tanto socket cliente como servidor
            shutdown(*d->other->fd, SHUT_RD);
            d->other->duplex &= ~READ;
        }
    } else {
        updateReadPtr(buffer, n);
    }
    logDebug("duplex WRITE interest: %d", d->duplex);

    copyComputeInterests(key->mux, d);
    copyComputeInterests(key->mux, d->other);
    if(d->duplex == NO_INTEREST) {
        ret = DONE;
    }
    return ret;
}


