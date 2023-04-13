//Cola protegida frente accesos simultáneos
#include <stdlib.h>
#include <threads.h>
#include <stdio.h>

// circular array
typedef struct _queue {
    int size;
    int used;
    int first;
    void **data;
    mtx_t mutex;
    cnd_t NoLlena; //Avisar que la cola ya no está llena
    cnd_t NoVacia; //Avisar que la cola ya no está vacia
    int desbloqueoRemove;
} _queue;

#include "queue.h"

queue q_create(int size) {
    queue q = malloc(sizeof(_queue));

    q->size  = size;
    q->used  = 0;
    q->first = 0;
    q->data  = malloc(size * sizeof(void *));
    q->desbloqueoRemove = 0;
    mtx_init(&q->mutex,mtx_plain);
    cnd_init(&q->NoLlena);
    cnd_init(&q->NoVacia);

    return q;
}

void q_desbloqueo(queue q){ //Desactiva el bloqueo de q_remove() cuando además la cola esté vacía
	mtx_lock(&q->mutex);
	q->desbloqueoRemove = 1;
	cnd_broadcast(&q->NoVacia);
	mtx_unlock(&q->mutex);
}

int q_elements(queue q) {
    return q->used;
}

int q_insert(queue q, void *elem) { //Bloquea hasta que haya espacio

    int estaba_vacia;
    mtx_lock(&q->mutex);
    while(q->size==q->used) {
        cnd_wait(&q->NoLlena, &q->mutex);
    }
    estaba_vacia = (q->used==0);
    q->data[(q->first+q->used) % q->size] = elem;
    q->used++;
    if(estaba_vacia){
        cnd_broadcast(&q->NoVacia);
    }
    mtx_unlock(&q->mutex);
    return 0;
}

void *q_remove(queue q) { //Bloquea mientras esté vacía y no se active el desbloqueo

    void *res;
    int estaba_llena;

    mtx_lock(&q->mutex);
    while(q->used==0 && !q->desbloqueoRemove){
        cnd_wait(&q->NoVacia, &q->mutex);
    }

    if(q->desbloqueoRemove && q->used==0) {
        mtx_unlock(&q->mutex);
        return NULL;
    }

    estaba_llena = (q->used==q->size);
    res=q->data[q->first];
    q->first=(q->first+1) % q->size;
    q->used--;
    if(estaba_llena){
        cnd_broadcast(&q->NoLlena);
    }
    mtx_unlock(&q->mutex);

    return res;
}

void q_destroy(queue q) {
    free(q->data);
    mtx_destroy(&q->mutex);
    cnd_destroy(&q->NoLlena);
    cnd_destroy(&q->NoVacia);
    free(q);
}